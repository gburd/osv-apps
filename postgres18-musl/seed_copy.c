/*
 * seed_copy -- recursively copy SRC into DST, preserving file modes.
 *
 * OSv boots PostgreSQL's data directory from a read-only baked cluster on the
 * image filesystem (rofs).  PostgreSQL must write to its data directory, so at
 * boot we copy the baked cluster (/data-seed) into a writable ramfs (/data)
 * before starting the postmaster.  cp -a is not available on the OSv image,
 * so this tiny helper does the copy with nothing but libc.
 *
 * Usage: seed_copy <src-dir> <dst-dir>
 */
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int copy_file(const char *src, const char *dst, mode_t mode)
{
	int in = open(src, O_RDONLY);
	if (in < 0) { perror(src); return -1; }
	int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, mode & 0777);
	if (out < 0) { perror(dst); close(in); return -1; }

	char buf[65536];
	ssize_t n;
	while ((n = read(in, buf, sizeof buf)) > 0) {
		char *p = buf;
		while (n > 0) {
			ssize_t w = write(out, p, n);
			if (w < 0) { perror(dst); close(in); close(out); return -1; }
			p += w; n -= w;
		}
	}
	if (n < 0) { perror(src); close(in); close(out); return -1; }
	close(in);
	close(out);
	return 0;
}

static int copy_tree(const char *src, const char *dst)
{
	struct stat st;
	if (lstat(src, &st) < 0) { perror(src); return -1; }

	if (S_ISDIR(st.st_mode)) {
		if (mkdir(dst, st.st_mode & 0777) < 0 && errno != EEXIST) {
			perror(dst);
			return -1;
		}
		DIR *d = opendir(src);
		if (!d) { perror(src); return -1; }
		struct dirent *e;
		int rc = 0;
		while ((e = readdir(d))) {
			if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
				continue;
			/* .keep is a directory-preservation marker (OSv manifest globs
			 * drop empty dirs); the dir is now created, skip the marker. */
			if (!strcmp(e->d_name, ".keep"))
				continue;
			char s[4096], t[4096];
			snprintf(s, sizeof s, "%s/%s", src, e->d_name);
			snprintf(t, sizeof t, "%s/%s", dst, e->d_name);
			if (copy_tree(s, t) < 0) rc = -1;
		}
		closedir(d);
		return rc;
	}
	return copy_file(src, dst, st.st_mode);
}

int main(int argc, char **argv)
{
	if (argc != 3) {
		fprintf(stderr, "usage: %s <src-dir> <dst-dir>\n", argv[0]);
		return 2;
	}
	if (copy_tree(argv[1], argv[2]) < 0) {
		fprintf(stderr, "seed_copy: failed copying %s -> %s\n", argv[1], argv[2]);
		return 1;
	}
	return 0;
}
