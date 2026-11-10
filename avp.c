
#define _DEFAULT_SOURCE

#include <stdio.h>
#include <archive.h>
#include <archive_entry.h>
#include <curl/curl.h>
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

#define CONTROL_PATH "/avp/CONTROL"

#define ETC_AVP             "/etc/avp/"
#define REPO_FILE           ETC_AVP "repo.list"
#define REPO_FOLDER         ETC_AVP "repositories"

#define PACKAGES_LIST       ETC_AVP "packages.list"
#define PACKAGES_FOLDER     ETC_AVP "packages"

#define VAR_LIB_AVP         "/var/lib/avp/"
#define SYNC_FOLDER         VAR_LIB_AVP "sync"
#define CACHE_FOLDER        VAR_LIB_AVP "cache"

#define VAR_CACHE_AVP       "/var/cache/avp/"
#define DOWNLOAD_FOLDER     VAR_CACHE_AVP "downloads"

#define MAX_LEN 256
#define MAX_PACKAGE 2096
#define MAX_REPOSITORIES 128

typedef struct {
    char name[MAX_LEN];
    char description[MAX_LEN];
    char arch[MAX_LEN];
    char version[MAX_LEN];
    char depends[MAX_PACKAGE];
    char conflicts[MAX_PACKAGE];
    char maintainder[MAX_LEN];
} Package;

typedef struct {
    char name[MAX_LEN];
    char url[MAX_LEN];
} Repository;

int file_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

int mkdir_p(const char *path)
{
    char buffer[1024];
    size_t len;

    if (!path || strlen(path) >= sizeof(buffer))
        return 1;

    snprintf(buffer, sizeof(buffer), "%s", path);
    len = strlen(buffer);

    if (len == 0)
        return 1;

    if (buffer[len - 1] == '/')
        buffer[len - 1] = '\0';

    for (char *p = buffer + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';

            if (mkdir(buffer, 0755) != 0 && errno != EEXIST)
                return 1;

            *p = '/';
        }
    }

    if (mkdir(buffer, 0755) != 0 && errno != EEXIST)
        return 1;

    return 0;
}

void cmpawr(char *key, char *val, Package *pkg)
{
    if (strcmp(key, "name") == 0)
        snprintf(pkg->name, sizeof(pkg->name), "%s", val);

    if (strcmp(key, "description") == 0)
        snprintf(pkg->description, sizeof(pkg->description), "%s", val);

    if (strcmp(key, "arch") == 0)
        snprintf(pkg->arch, sizeof(pkg->arch), "%s", val);

    if (strcmp(key, "version") == 0)
        snprintf(pkg->version, sizeof(pkg->version), "%s", val);

    if (strcmp(key, "depends") == 0)
        snprintf(pkg->depends, sizeof(pkg->depends), "%s", val);

    if (strcmp(key, "conflicts") == 0)
        snprintf(pkg->conflicts, sizeof(pkg->conflicts), "%s", val);

    if (strcmp(key, "maintainder") == 0)
        snprintf(pkg->maintainder, sizeof(pkg->maintainder), "%s", val);
}

int parse_package(const char *control, Package *pkg)
{
    FILE *file = fopen(control, "r");

    if (!file)
        return 0;

    memset(pkg, 0, sizeof(*pkg));

    char line[512];

    while (fgets(line, sizeof(line), file)) {
        line[strcspn(line, "\r\n")] = '\0';

        char *eq = strchr(line, '=');

        if (!eq)
            continue;

        *eq = '\0';

        cmpawr(line, eq + 1, pkg);
    }

    fclose(file);

    if (!pkg->name[0] ||
        !pkg->arch[0] ||
        !pkg->version[0] ||
        !pkg->maintainder[0])
        return 0;

    return 1;
}

int validate_package(const char *folder, Package *pkg)
{
    char control_file[512];

    snprintf(control_file, sizeof(control_file),
             "%s%s", folder, CONTROL_PATH);

    if (!file_exists(control_file)) {
        fprintf(stderr, "avp: %s not found\n", control_file);
        return 1;
    }

    if (!parse_package(control_file, pkg)) {
        fprintf(stderr, "avp: invalid control file\n");
        return 1;
    }

    return 0;
}

int add_to_archive(struct archive *a,
                   const char *filename,
                   const char *prefix)
{
    char path[1024];

    snprintf(path, sizeof(path), "%s/%s", prefix, filename);

    struct stat st;

    if (lstat(path, &st) != 0)
        return 1;

    struct archive_entry *entry = archive_entry_new();

    if (!entry)
        return 1;

    archive_entry_set_pathname(entry, path);
    archive_entry_set_size(entry, S_ISREG(st.st_mode) ? st.st_size : 0);
    archive_entry_set_filetype(entry, st.st_mode);
    archive_entry_set_perm(entry, st.st_mode & 07777);

    if (archive_write_header(a, entry) != ARCHIVE_OK) {
        fprintf(stderr, "avp: tar: %s\n", archive_error_string(a));
        archive_entry_free(entry);
        return 1;
    }

    if (S_ISREG(st.st_mode)) {
        FILE *f = fopen(path, "rb");

        if (!f) {
            archive_entry_free(entry);
            return 1;
        }

        char buff[8192];
        size_t len;

        while ((len = fread(buff, 1, sizeof(buff), f)) > 0) {
            if (archive_write_data(a, buff, len) < 0) {
                fclose(f);
                archive_entry_free(entry);
                return 1;
            }
        }

        fclose(f);
    }

    archive_entry_free(entry);

    if (S_ISDIR(st.st_mode)) {
        DIR *dir = opendir(path);

        if (!dir)
            return 1;

        struct dirent *dp;

        while ((dp = readdir(dir)) != NULL) {
            if (strcmp(dp->d_name, ".") == 0 ||
                strcmp(dp->d_name, "..") == 0)
                continue;

            if (add_to_archive(a, dp->d_name, path) != 0) {
                closedir(dir);
                return 1;
            }
        }

        closedir(dir);
    }

    return 0;
}

int create_tar(const char *outname, const char *dirname)
{
    struct archive *a = archive_write_new();

    if (!a)
        return 1;

    archive_write_set_format_ustar(a);

    if (archive_write_open_filename(a, outname) != ARCHIVE_OK) {
        fprintf(stderr, "avp: %s\n", archive_error_string(a));
        archive_write_free(a);
        return 1;
    }

    DIR *dir = opendir(dirname);

    if (!dir) {
        archive_write_close(a);
        archive_write_free(a);
        return 1;
    }

    struct dirent *dp;
    int failed = 0;

    while ((dp = readdir(dir)) != NULL) {
        if (strcmp(dp->d_name, ".") == 0 ||
            strcmp(dp->d_name, "..") == 0)
            continue;

        if (add_to_archive(a, dp->d_name, dirname) != 0)
            failed = 1;
    }

    closedir(dir);

    archive_write_close(a);
    archive_write_free(a);

    return failed;
}

int extract_tar(const char *filename, const char *dest)
{
    struct archive *a = archive_read_new();
    struct archive *ext = archive_write_disk_new();
    struct archive_entry *entry;

    if (!a || !ext)
        return 1;

    int flags = ARCHIVE_EXTRACT_TIME |
                ARCHIVE_EXTRACT_PERM |
                ARCHIVE_EXTRACT_ACL |
                ARCHIVE_EXTRACT_FFLAGS;

    archive_read_support_format_all(a);
    archive_read_support_filter_all(a);

    archive_write_disk_set_options(ext, flags);
    archive_write_disk_set_standard_lookup(ext);

    if (archive_read_open_filename(a, filename, 10240) != ARCHIVE_OK) {
        fprintf(stderr, "avp: %s\n", archive_error_string(a));
        archive_read_free(a);
        archive_write_free(ext);
        return 1;
    }

    int r;
    int failed = 0;

    while ((r = archive_read_next_header(a, &entry)) != ARCHIVE_EOF) {
        if (r < ARCHIVE_OK) {
            fprintf(stderr, "avp: %s\n", archive_error_string(a));

            if (r < ARCHIVE_WARN) {
                failed = 1;
                break;
            }
        }

        const char *current = archive_entry_pathname(entry);
        char fpath[1024];

        if (dest) {
            if (!current || current[0] == '/') {
                fprintf(stderr, "avp: invalid archive path\n");
                failed = 1;
                break;
            }

            char *check = strdup(current);

            if (!check) {
                failed = 1;
                break;
            }

            char *saveptr;
            char *part = strtok_r(check, "/", &saveptr);

            while (part) {
                if (strcmp(part, "..") == 0) {
                    failed = 1;
                    break;
                }

                part = strtok_r(NULL, "/", &saveptr);
            }

            free(check);

            if (failed)
                break;

            int n = snprintf(fpath, sizeof(fpath), "%s/%s", dest, current);

            if (n < 0 || (size_t)n >= sizeof(fpath)) {
                failed = 1;
                break;
            }

            archive_entry_set_pathname(entry, fpath);
        }

        if (archive_write_header(ext, entry) < ARCHIVE_OK) {
            fprintf(stderr, "avp: %s\n", archive_error_string(ext));
            failed = 1;
        }

        const void *buff;
        size_t size;
        la_int64_t offset;

        while ((r = archive_read_data_block(a, &buff, &size, &offset)) == ARCHIVE_OK) {
            if (archive_write_data_block(ext, buff, size, offset) < ARCHIVE_OK) {
                fprintf(stderr, "avp: %s\n", archive_error_string(ext));
                failed = 1;
                break;
            }
        }

        if (r != ARCHIVE_EOF && r < ARCHIVE_OK) {
            failed = 1;
            break;
        }

        archive_write_finish_entry(ext);
    }

    archive_read_close(a);
    archive_read_free(a);

    archive_write_close(ext);
    archive_write_free(ext);

    return failed;
}

int build_pkg(const char *foldername)
{
    Package pkg;

    if (validate_package(foldername, &pkg))
        return 1;

    char outname[512];

    printf("Building %s...\n", pkg.name);

    snprintf(outname, sizeof(outname), "%s.avp", foldername);

    return create_tar(outname, foldername);
}

int parse_repositorys(Repository *repository)
{
    FILE *file = fopen(REPO_FILE, "r");

    if (!file)
        return -1;

    char line[1024];
    int count = 0;

    while (fgets(line, sizeof(line), file)) {
        char *saveptr;
        char *name = strtok_r(line, " \t\r\n", &saveptr);

        if (!name || name[0] == '#')
            continue;

        char *url = strtok_r(NULL, " \t\r\n", &saveptr);

        if (!url)
            continue;

        if (count >= MAX_REPOSITORIES)
            break;

        snprintf(repository[count].name,
                 sizeof(repository[count].name), "%s", name);

        snprintf(repository[count].url,
                 sizeof(repository[count].url), "%s", url);

        count++;
    }

    fclose(file);

    return count;
}

int download_sources(const char *url, const char *dest,
                    char *reason, size_t reason_size)
{
    char temporary[1024];
    char error[CURL_ERROR_SIZE] = {0};

    int n = snprintf(temporary, sizeof(temporary), "%s.tmp", dest);

    if (n < 0 || (size_t)n >= sizeof(temporary)) {
        snprintf(reason, reason_size, "Temporary path too long");
        return 1;
    }

    FILE *file = fopen(temporary, "wb");

    if (!file) {
        snprintf(reason, reason_size, "Cannot create temporary file: %s",
                 strerror(errno));
        return 1;
    }

    CURL *curl = curl_easy_init();

    if (!curl) {
        fclose(file);
        remove(temporary);
        snprintf(reason, reason_size, "Cannot initialize curl");
        return 1;
    }

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, file);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "avp/0.1");
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error);

    CURLcode res = curl_easy_perform(curl);

    long response = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response);

    curl_easy_cleanup(curl);

    if (fclose(file) != 0)
        res = CURLE_WRITE_ERROR;

    if (res != CURLE_OK || response != 200) {
        if (response >= 400) {
            snprintf(reason, reason_size, "HTTP error: %ld", response);
        } else {
            snprintf(reason, reason_size, "%s",
                     error[0] ? error : curl_easy_strerror(res));
        }

        remove(temporary);
        return 1;
    }

    if (rename(temporary, dest) != 0) {
        snprintf(reason, reason_size, "Cannot update index: %s",
                 strerror(errno));
        remove(temporary);
        return 1;
    }

    return 0;
}

int sync_repo(void)
{
    Repository repo[MAX_REPOSITORIES];

    int count = parse_repositorys(repo);

    if (count < 0) {
        puts("Error in repositories");
        puts("Cannot open repository configuration");
        return 1;
    }

    if (count == 0) {
        puts("Nothing to do!");
        return 0;
    }

    if (mkdir_p(SYNC_FOLDER)) {
        puts("Error in sync");
        puts("Cannot create sync directory");
        return 1;
    }

    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
        puts("Error in sync");
        puts("Cannot initialize curl");
        return 1;
    }

    int failed = 0;

    for (int i = 0; i < count; i++) {
        char url[MAX_LEN + 64];
        char directory[1024];
        char dest[1024];
        char reason[512] = {0};

        int valid_name = 1;

        for (size_t j = 0; repo[i].name[j]; j++) {
            char c = repo[i].name[j];

            if (!((c >= 'a' && c <= 'z') ||
                  (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') ||
                  c == '_' || c == '-')) {
                valid_name = 0;
                break;
            }
        }

        if (!valid_name) {
            printf("Error in %s\n", repo[i].name);
            puts("Invalid repository name");
            failed = 1;
            continue;
        }

        size_t url_len = strlen(repo[i].url);

        if (url_len == 0) {
            printf("Error in %s\n", repo[i].name);
            puts("Empty repository URL");
            failed = 1;
            continue;
        }

        int n = snprintf(url, sizeof(url), "%s%ssources.list",
                         repo[i].url,
                         repo[i].url[url_len - 1] == '/' ? "" : "/");

        if (n < 0 || (size_t)n >= sizeof(url)) {
            printf("Error in %s\n", repo[i].name);
            puts("Repository URL too long");
            failed = 1;
            continue;
        }

        snprintf(directory, sizeof(directory), "%s/%s",
                 SYNC_FOLDER, repo[i].name);

        snprintf(dest, sizeof(dest), "%s/sources.list", directory);

        printf("Sycing %s...\n", repo[i].name);
        fflush(stdout);

        if (mkdir_p(directory)) {
            printf("Error in %s\n", repo[i].name);
            printf("Cannot create directory: %s\n", strerror(errno));
            failed = 1;
            continue;
        }

        if (download_sources(url, dest, reason, sizeof(reason))) {
            printf("Error in %s\n", repo[i].name);
            puts(reason);
            failed = 1;
            continue;
        }
    }
    puts("All repositorys synced.");

    curl_global_cleanup();

    return failed;
}

int add_repository(const char *repository, const char *name)
{
    FILE *file = fopen(REPO_FILE, "a");
    if (!file) return 1;
    fprintf(file, "%s %s", name, repository);
    fclose(file);
    return 0;
}

int remove_repository(const char *name)
{
    FILE *file = fopen(REPO_FILE, "r");
    if (!file) return 1;

    FILE *tmp = fopen(REPO_FILE ".tmp", "w");
    if (!tmp) {
        fclose(file);
        return 1;
    }
    char line[1024];
    char repo_name[256];

    while (fgets(line, sizeof(line), file)) {
        if (sscanf(line, "%255s", repo_name) != 1)
            continue;

        if (strcmp(repo_name, name) != 0)
            fputs(line, tmp);
    }

    fclose(file);
    fclose(tmp);

    if (rename(REPO_FILE ".tmp", REPO_FILE) != 0)
        return 1;

    return 0;
}

void usage(void)
{
    puts("avp [COMMAND] [ARGS...]");
    puts("    -b [FOLDER]   Build a package");
    puts("    -e [FILE]     Extract a package");
    puts("    -S        Sync all repositories");
    puts("");
    puts("    --add-repo [NAME] [LINK]     Add a repository in avp");
    puts("    --del-repo [NAME]            Remove a repository in avp");
}

int main(int argc, const char *argv[])
{
    if (argc < 2) {
        usage();
        return 1;
    }
    if (geteuid() != 0) {
        puts("avp: are you root?");
        return 1;
    }

    const char *command = argv[1];
    if (argc >= 3 && strcmp(command, "-b") == 0)
        return build_pkg(argv[2]);

    if (argc >= 3 && strcmp(command, "-e") == 0)
        return extract_tar(argv[2], ".");

    if (argc >= 4 && strcmp(command, "--add-repo") == 0)
        return add_repository(argv[3], argv[2]);

    if (argc >= 3 && strcmp(command, "--del-repo") == 0)
        return remove_repository(argv[2]);

    if (strcmp(command, "-S") == 0)
        return sync_repo();
    
    usage();
    return 1;
}