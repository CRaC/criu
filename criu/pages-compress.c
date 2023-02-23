#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <sys/resource.h>
#include <unistd.h>

#include <compel/plugins/std/syscall-codes.h>

#include "pages-compress.h"

#include "crtools.h"
#include "image.h"
#include "log.h"
#include "memfd.h"
#include "sched.h"
#include "util.h"

#include <lz4/programs/lz4io.h>
#include <lz4/lib/lz4frame.h>

const char *temp_path_decompressed = "/tmp/decompressed";

int compress_images(void)
{
	char format[PATH_MAX];
	char pathsrc[PATH_MAX];
	char pathdst[PATH_MAX];
	const long pid = getpid();
	const int dfd = get_service_fd(IMG_FD_OFF);
	const int id = 1; // the only image we'd like to compress
	int ret;

	LZ4IO_prefs_t *lz4_prefs = LZ4IO_defaultPreferences();
	LZ4IO_setBlockSize(lz4_prefs, 128 * 1024);

	pr_debug("Compressing pages\n");

	sprintf(format, "%s/%s", "/proc/%ld/fd/%d", imgset_template[CR_FD_PAGES].fmt);
	sprintf(pathsrc, format, pid, dfd, id);

	sprintf(format, "%s/%s", "/proc/%ld/fd/%d", imgset_template[CR_FD_PAGES_COMP].fmt);
	sprintf(pathdst, format, pid, dfd, id);

	ret = LZ4IO_compressFilename(pathsrc, pathdst, 8, lz4_prefs);
	LZ4IO_freePreferences(lz4_prefs);

	if (ret) {
		pr_err("Can't compress %s to %s\n", pathsrc, pathdst);
		return -1;
	}
	if (unlink(pathsrc)) {
		pr_perror("Can't delete uncompressed source pages image: %s\n", pathsrc);
		return -1;
	}
	return 0;
}

int decompress_image(int comp_fd, const char *path) {

	const int compbufsize = 64 * 1024;
	char compbuf[compbufsize];
	const int outbufsize = 4 * 1024 * 1024; // LZ4F_max4MB is the max block size; see LZ4F_blockSizeID_t for details
	char outbuf[outbufsize];
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

	pr_debug("Creating file at '%s'\n", temp_path_decompressed);
	ret = open(temp_path_decompressed, O_CREAT | O_WRONLY | O_TRUNC, 0600);

	if (0 > ret) {
		pr_err("Can't create file, errno=%d\n", errno);
		LZ4F_freeDecompressionContext(dctx);
		return -1;
	}

	while (1) {
		const size_t readbytes = read(comp_fd, compbuf + offset, bytestoread);
		totalread += readbytes;
		if (!readbytes && 0 == offset) {
			/* reached end of file or stream */
			break;
		}
		{
			size_t outsize = sizeof outbuf;
			size_t insize = offset + readbytes;
			const size_t insize_orig = insize;
			lz4err = LZ4F_decompress(dctx, outbuf, &outsize, compbuf, &insize, NULL);
			if (LZ4F_isError(lz4err)) {
				pr_err("LZ4 Decompress error: %s\n", LZ4F_getErrorName(lz4err));
				break;
			}
			{
				const ssize_t res = write(ret, outbuf, outsize);
				if (0 > res) {
					pr_err("write error to output file, fd=%d, errno=%d\n", ret, errno);
					break;
				}
			}
			totalwrite += outsize;
			offset = insize_orig - insize;
			bytestoread = insize;
			memmove(compbuf, compbuf + compbufsize - offset, offset);
		}
	}
	close(ret);
	pr_debug("decompression completed, read %lu, wrote %lu\n", (unsigned long)totalread, (unsigned long)totalwrite);
	return 0;
}

static pthread_t decomp_thread = 0;

static void *decompression_thread_routine(void *param) {
    int ret;
    const char *image_path = "pages-1.comp.img";
    const int dfd = get_service_fd(IMG_FD_OFF);
    const int comp_fd = openat(dfd, image_path, O_RDONLY, CR_FD_PERM);
    if (0 > comp_fd) {
        pr_err("Can't open '%s' for dfd=%d, errno=%d\n", image_path, dfd, errno);
        return NULL;
    }
    ret = decompress_image(comp_fd, image_path);
    close(comp_fd);
    if (0 > ret) {
        pr_err("Failed to decompress image, ret=%d\n", ret);
    }
	pr_info("Decompression thread completed\n");
    return NULL;
}

int decompression_thread_start(void) {
    int ret;
    if (decomp_thread) {
        pr_err("Decompression thread already started\n");
        return -1;
    }

	pr_debug("Starting decompression thread...\n");
	{
		// Preventing processing SIGCHLD by the thread,
		// because SIGCHLD mask is changed by CRIU's main thread during restore.
		sigset_t blockmask, oldmask;
		sigemptyset(&blockmask);
		sigaddset(&blockmask, SIGCHLD);
		if (sigprocmask(SIG_BLOCK, &blockmask, &oldmask) == -1) {
			pr_perror("Cannot set mask of blocked signals");
			return -1;
		}
		ret = pthread_create(&decomp_thread, NULL, decompression_thread_routine, NULL);
		if (sigprocmask(SIG_SETMASK, &oldmask, NULL) == -1) {
			pr_perror("Can not unset mask of blocked signals");
		}
		if (ret) {
			pr_err("Can't start decompression thread, pthread_create returned %d\n", ret);
			return ret;
		}
	}
	pr_debug("Decompression thread started, decomp_thread=%lu\n", decomp_thread);
    return 0;
}

int decompression_thread_join(void) {
	int ret;
    if (!decomp_thread) {
        pr_err("Decompression thread doesn't exist\n");
        return -1;
    }
	ret = pthread_join(decomp_thread, NULL);
	if (ret) {
        pr_err("Decompression thread joining failed, ret=%d\n", ret);
		return -1;
	}
	decomp_thread = 0;
	pr_debug("Decompression thread joined\n");
    return 0;
}

int decompression_get_fd(void) {
	int ret;
	pr_debug("wait for semaphore...\n");
	ret = open(temp_path_decompressed, O_RDONLY, 0600);
	if (0 > ret) {
		pr_err("Failed open %s errno=%d\n", temp_path_decompressed, errno);
	}
	return ret;
}
