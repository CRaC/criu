
#include <limits.h>
#include <stdio.h>
#include <unistd.h>

#include "pages-compress.h"
#include "image.h"
#include "log.h"
#include "memfd.h"
#include "util.h"

#include <lz4/programs/lz4io.h>
#include <lz4/lib/lz4frame.h>

unsigned pages_image_max_id(void);

int compress_images(void)
{
	char format[PATH_MAX];
	char pathsrc[PATH_MAX];
	char pathdst[PATH_MAX];
	int id;

	const int max_id = pages_image_max_id();

	const long pid = getpid();
	const int dfd = get_service_fd(IMG_FD_OFF);

	LZ4IO_prefs_t *lz4_prefs = LZ4IO_defaultPreferences();
	LZ4IO_setBlockSize(lz4_prefs, 128 * 1024);

	pr_debug("Compressing pages\n");
	LZ4IO_setNotificationLevel(100);
	for (id = 1; id < max_id; ++id) {
		sprintf(format, "%s/%s", "/proc/%ld/fd/%d", imgset_template[CR_FD_PAGES].fmt);
		sprintf(pathsrc, format, pid, dfd, id);

		sprintf(format, "%s/%s", "/proc/%ld/fd/%d", imgset_template[CR_FD_PAGES_COMP].fmt);
		sprintf(pathdst, format, pid, dfd, id);

		if (LZ4IO_compressFilename(pathsrc, pathdst, 8, lz4_prefs)) {
			LZ4IO_freePreferences(lz4_prefs);
			pr_err("Can't compress %s to %s\n", pathsrc, pathdst);
			return -1;
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

int compress_image(const char *pathsrc, const char *pathdst) {

    const int LZ4_Compression_Level = 8;
	LZ4IO_prefs_t *lz4_prefs = LZ4IO_defaultPreferences();
    LZ4IO_setBlockSize(lz4_prefs, 128 * 1024);// * 1024);
    LZ4IO_setNotificationLevel(100);

    if (LZ4IO_compressFilename(pathsrc, pathdst, LZ4_Compression_Level, lz4_prefs)) {
        LZ4IO_freePreferences(lz4_prefs);
        pr_err("Can't compress %s to %s\n", pathsrc, pathdst);
        return -1;
    }
    return 0;
}

int decompress_image(int comp_fd, const char *path) {

	const int compbufsize = 64 * 1024;
	char compbuf[compbufsize];
	char outbuf[4 * compbufsize];
	LZ4F_errorCode_t lz4err;
	LZ4F_decompressionContext_t dctx;
	int ret;
	size_t totalread = 0;
	size_t totalwrite = 0;
	int bytestoread = compbufsize;
	size_t offset = 0;

	lz4err = LZ4F_createDecompressionContext(&dctx, LZ4F_VERSION);
	if (LZ4F_isError(lz4err)) {
		pr_err("Can't create LZ4 decompression context\n");
		return -1;
	}

	ret = memfd_create(path, 0);
	if (0 > ret) {
		pr_err("Can't create memfd, errno=%d\n", errno);
		LZ4F_freeDecompressionContext(dctx);
		return -1;
	}

	while (1) {
		size_t readbytes;
		readbytes = read(comp_fd, compbuf + offset, bytestoread);
		totalread += readbytes;
		// pr_debug("read %16lu bytes, total read %16lu\n", (unsigned long)readbytes, (unsigned long)totalread);
		if (!readbytes) {
			/* reached end of file or stream */
			break;
		}
		{
			size_t outsize = sizeof outbuf;
			size_t insize = offset + readbytes;
			const size_t insize_orig = insize;
			pr_debug("    pos %lu, in %lu\n", (unsigned long)totalread, (unsigned long)insize);
			lz4err = LZ4F_decompress(dctx, outbuf, &outsize, compbuf, &insize, NULL);
			if (!LZ4F_isError(lz4err)) {
				{
					ssize_t res = write(ret, outbuf, outsize);
					if (0 > res) {
						pr_err("write error to output file\n");
						break;
					}
					pr_debug("written %d bytes to output file\n", (int)res);
				}
				totalwrite += outsize;
				offset = insize_orig - insize;
				// bytestoread = compbufsize; //min((unsigned)lz4err, (unsigned)compbufsize);
				bytestoread = insize;
				pr_debug(
					"    consumed %lu, decompressed %lu, next read %lu, offset %lu, total write %lu\n",
					(unsigned long)insize, (unsigned long)outsize,
					(unsigned long)lz4err, (unsigned long)offset,
					(unsigned long)totalwrite);
				memmove(compbuf, compbuf + compbufsize - offset, offset);
			} else {
				pr_err("LZ4 Decompress error: %s\n", LZ4F_getErrorName(lz4err));
				break;
			}
		}
	}
	pr_debug("decompression completed, read %lu, wrote %lu\n", (unsigned long)totalread, (unsigned long)totalwrite);
	lseek(ret, 0, SEEK_SET);

	return ret;
}
