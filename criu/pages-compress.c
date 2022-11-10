
#include <limits.h>
#include <stdio.h>
#include <unistd.h>

#include "image.h"
#include "util.h"
// #include "pages-compress.h"

#include <lz4/programs/lz4io.h>

unsigned pages_image_max_id(void);

int pages_compress(void)
{
	char format[PATH_MAX];
	char pathsrc[PATH_MAX];
	char pathdst[PATH_MAX];
	char pathdst2[PATH_MAX];
	int id;

	const int max_id = pages_image_max_id();

	const long pid = getpid();
	const int dfd = get_service_fd(IMG_FD_OFF);

	LZ4IO_prefs_t *lz4_prefs = LZ4IO_defaultPreferences();
    LZ4IO_setBlockSize(lz4_prefs, 128 * 1024);// * 1024);

    pr_debug("Compressing pages\n");
    LZ4IO_setNotificationLevel(100);
	for (id = 1; id < max_id; ++id) {
		sprintf(format, "%s/%s", "/proc/%ld/fd/%d", imgset_template[CR_FD_PAGES].fmt);
		sprintf(pathsrc, format, pid, dfd, id);

		sprintf(format, "%s/%s", "/proc/%ld/fd/%d", imgset_template[CR_FD_PAGES_COMP].fmt);
		sprintf(pathdst, format, pid, dfd, id);

		sprintf(format, "%s/%s.out", "/proc/%ld/fd/%d", imgset_template[CR_FD_PAGES].fmt);
		sprintf(pathdst2, format, pid, dfd, id);

    	printf("XXX %s:%d: (%5d) %s: pathsrc=%s, pathdst=%s\n", __FILE__, __LINE__, getpid(), __FUNCTION__, pathsrc, pathdst);
		if (LZ4IO_compressFilename(pathsrc, pathdst, 8, lz4_prefs)) {
			LZ4IO_freePreferences(lz4_prefs);
			pr_err("Can't compress %s to %s\n", pathsrc, pathdst);
			return -1;
		}
        {
    	    LZ4IO_prefs_t *lz4_prefs_decomp = LZ4IO_defaultPreferences();
            LZ4IO_setBlockSize(lz4_prefs_decomp, 64 * 1024);// * 1024);
            if (LZ4IO_decompressFilename(pathdst, pathdst2, lz4_prefs_decomp)) {
                LZ4IO_freePreferences(lz4_prefs_decomp);
                pr_err("Can't decompress %s to %s\n", pathdst, pathdst2);
                return -1;
            }
            LZ4IO_freePreferences(lz4_prefs_decomp);
        }
        {
            int res;
            char command[4096];
            sprintf(command, "md5sum %s", pathsrc);
            res = system(command);
            if (res) {
                pr_err("XXX failed %s\n", command);
            }
            sprintf(command, "md5sum %s", pathdst2);
            res = system(command);
            if (res) {
                pr_err("XXX failed %s\n", command);
            }
            sprintf(command, "ls -l %s*", pathsrc);
            res = system(command);
            if (res) {
                pr_err("XXX failed %s\n", command);
            }

            if (unlink(pathdst2)) {
                LZ4IO_freePreferences(lz4_prefs);
                pr_perror("Can't delete uncompressed source pages image: %s\n", pathdst2);
                return -1;
            }

            {
                const char *ff[1] = {pathdst};
                LZ4IO_displayCompressedFilesInfo(ff, 1);
            }
        }

		if (unlink(pathsrc)) {
			LZ4IO_freePreferences(lz4_prefs);
			pr_perror("Can't delete uncompressed source pages image: %s\n", pathsrc);
			return -1;
		}
	}
	LZ4IO_freePreferences(lz4_prefs);
	return 0;
}
