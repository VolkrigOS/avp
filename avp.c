#define _DEFAULT_SOURCE

#include <stdio.h>
#include <archive.h>
#include <dirent.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <archive_entry.h>

#define control_MAX 2048
#define control_PATH "/avp/CONTROL"
#define MAX_LEN 256
#define MAX_PACKAGE 2096

typedef struct {
	char name[MAX_LEN];
  	char description[MAX_LEN];
	char arch[MAX_LEN];
	char version[MAX_LEN];

	char depends[MAX_PACKAGE];
	char conflicts[MAX_PACKAGE];
	char maintainder[MAX_LEN];
} Package;

int file_exists(const char *path)
{
	struct stat buffer;
	return (stat(path, &buffer) == 0);
}

void cmpawr(char *key, char *val, Package *pkg)
{
	if (strcmp(key, "name") == 0) strncpy(pkg->name, val, MAX_LEN-1);
	if (strcmp(key, "description") == 0) strncpy(pkg->description, val, MAX_LEN-1);
	if (strcmp(key, "arch") == 0) strncpy(pkg->arch, val, MAX_LEN-1);
	if (strcmp(key, "version") == 0) strncpy(pkg->version, val, MAX_LEN-1);
	if (strcmp(key, "depends") == 0) strncpy(pkg->depends, val, MAX_LEN-1);
	if (strcmp(key, "conflicts") == 0) strncpy(pkg->conflicts, val, MAX_LEN-1);
	if (strcmp(key, "maintainder") == 0) strncpy(pkg->maintainder, val, MAX_LEN-1);
}


int parse_package(const char *control, Package *pkg)
{
	FILE *file = fopen(control, "r");
	if (!file) return 0;

	memset(pkg, 0, sizeof(Package));

	char line[512];
	while (fgets(line, sizeof(line), file)) {
		line[strcspn(line, "\r\n")] = 0;
		char *eq = strchr(line, '=');
		if (eq != NULL) {
			*eq = '\0';
			char *key = line;
			char *val = eq + 1;
			cmpawr(key, val, pkg);
		}
	}

	fclose(file);
	if (strlen(pkg->name) == 0 || strlen(pkg->arch) == 0 || strlen(pkg->version) == 0 || strlen(pkg->maintainder) == 0) return 0;
	return 1;
}

int validate_package(const char *folder, Package *pkg)
{
	char control_file[512];
	snprintf(control_file, sizeof(control_file), "%s/%s", folder, control_PATH);

	if (!file_exists(control_file))
	{
		printf("avp: %s file was not founded\n", control_file);
		return 1;
	};

	FILE *file = fopen(control_file, "r");
	if (!file)
	{
		printf("avp: %s was not founded in your package\n", control_PATH);
		return 1;
	};

	if (parse_package(control_file, pkg) == 0)
	{
		printf("avp: invalid control file\n");
		return 1;
	};
	return 0;
}

int add_to_archive(struct archive *a, const char *filename, const char *prefix)
{
	char path[1024];
	snprintf(path, sizeof(path), "%s/%s", prefix, filename);

	struct stat st;
	if (lstat(path, &st) != 0) return 0;

	struct archive_entry *entry = archive_entry_new();
	archive_entry_set_pathname(entry, path);
	archive_entry_set_size(entry, st.st_size);
	archive_entry_set_filetype(entry, st.st_mode);
	archive_entry_set_perm(entry, st.st_mode);
	archive_write_header(a, entry);

	if (S_ISREG(st.st_mode)) {
		FILE *f = fopen(path, "rb");
		if (f) {
			char *buff[8192];
			size_t len;
			while ((len = fread(buff, 1, sizeof(buff), f)) > 0) {
				archive_write_data(a, buff, len);
			}
			fclose(f);
		}
	}
	archive_entry_free(entry);
	if (S_ISDIR(st.st_mode)) {
		DIR *dir = opendir(path);
		if (dir) {
			struct dirent *dp;
			while ((dp = readdir(dir)) != NULL) {
				if (strcmp(dp->d_name, ".") == 0 || strcmp(dp->d_name, "..") == 0) continue;
				add_to_archive(a, dp->d_name, path);
			}
			closedir(dir);
		}
	}
	return 0;
}

int create_tar(const char *outname, const char *dirname)
{
	struct archive *a;

	a = archive_write_new();
	archive_write_set_format_ustar(a);
	archive_write_open_filename(a, outname);

	DIR *dir = opendir(dirname);
	if (dir)
	{
		struct dirent *dp;
		while ((dp = readdir(dir)) != NULL)
		{
			if (strcmp(dp->d_name, ".") == 0 || strcmp(dp->d_name, "..") == 0) continue;
			add_to_archive(a, dp->d_name, dirname);
		}
		closedir(dir);
	}
	archive_write_close(a);
	archive_write_free(a);
	return 0;
}

void usage(void)
{
	puts("avp [COMMAND] [ARGS...]");
	puts("	-b [FOLDER]	Build to a package");
}

int build_pkg(const char *foldername)
{
	Package pkg;
	if (validate_package(foldername, &pkg) == 1) return 1;
	char outname[512];

	printf("Building %s...\n", pkg.name);
	snprintf(outname, sizeof(outname), "%s.avp", foldername);
	return create_tar(outname, foldername);
}

int main(int argc, const char *argv[])
{
	if (argc < 2)
	{
		usage();
		return 1;
	}

	const char *command = argv[1];

	if (argc >= 3 && strcmp(command, "-b") == 0) return build_pkg(argv[2]);
	usage();
	return 0;
}
