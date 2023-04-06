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

#undef LOG_PREFIX
#define LOG_PREFIX "decompress: "

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

	snprintf(format, sizeof(format), "%s/%s", "/proc/%ld/fd/%d", imgset_template[CR_FD_PAGES].fmt);
	snprintf(pathsrc, sizeof(pathsrc), format, pid, dfd, id);

	snprintf(format, sizeof(format), "%s/%s", "/proc/%ld/fd/%d", imgset_template[CR_FD_PAGES_COMP].fmt);
	snprintf(pathdst, sizeof(pathdst), format, pid, dfd, id);

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

static int decompress_image(int comp_fd) {

	const int compbufsize = 64 * 1024;
	char compbuf[compbufsize];
	const int outbufsize = 4 * 1024 * 1024; // LZ4F_max4MB is the max block size; see LZ4F_blockSizeID_t for details
	char outbuf[outbufsize];
	LZ4F_errorCode_t lz4err;
	LZ4F_decompressionContext_t dctx;
	int decomp_fd;
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
	decomp_fd = open(temp_path_decompressed, O_CREAT | O_WRONLY | O_TRUNC, S_IRUSR | S_IWUSR);

	if (0 > decomp_fd) {
		pr_err("Can't create file, errno=%d\n", errno);
		LZ4F_freeDecompressionContext(dctx);
		return -1;
	}

	while (1) {
		const ssize_t readbytes = read(comp_fd, compbuf + offset, bytestoread);
		if (0 > readbytes) {
			if (EINTR == errno) {
				continue;
			}
			pr_perror("failed to read compressed data");
			break; // stop on error
		}
		if (!readbytes && 0 == offset) {
			/* reached end of file or stream */
			break;
		}
		totalread += readbytes;
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
				// Write decompressed bytes
				ssize_t bytes_written = 0;
				ssize_t out_offset = 0;
				do {
					bytes_written = write(decomp_fd, outbuf + out_offset, outsize);
					if (0 > bytes_written) {
						if (EINTR == errno) {
							continue;
						}
						pr_perror ("write error to decompressed file");
						break;
					}
					out_offset += bytes_written;
					if (outsize < (size_t)bytes_written) {
						pr_err("written more bytes in total than needed, extected=%lu, written=%lu\n",
						       (unsigned long)outsize, (unsigned long)bytes_written);
						break;
					}
					outsize -= bytes_written;
					totalwrite += bytes_written;
				} while (0 < outsize);
			}
			if (0 < outsize) {
				pr_err("writing is not completed, outsize=%lu\n", (unsigned long)outsize);
				break;
			}
			offset = insize_orig - insize;
			bytestoread = insize;
			memmove(compbuf, compbuf + compbufsize - offset, offset);
		}
	}
	if (0 > close(decomp_fd)) {
		pr_perror("failed closing decompressed file");
	}
	LZ4F_freeDecompressionContext(dctx);
	pr_debug("decompression completed, read %lu, wrote %lu\n", (unsigned long)totalread, (unsigned long)totalwrite);
	return 0;
}

static pthread_t decomp_thread = 0;

static void *decompression_thread_routine(void *param) {
	int ret;
	int comp_fd = (intptr_t)param;
	ret = decompress_image(comp_fd);
	close(comp_fd);
	if (0 > ret) {
		pr_err("Failed to decompress image, ret=%d\n", ret);
	}
	pr_info("Decompression thread completed\n");
	return NULL;
}

int decompression_thread_start(void) {
	int ret;
	int comp_fd;
	char image_path[PATH_MAX];
	const int dfd = get_service_fd(IMG_FD_OFF);

	if (decomp_thread) {
		pr_err("Decompression thread already started\n");
		return -1;
	}

	pr_debug("Opening compressed image...\n");
	snprintf(image_path, sizeof(image_path), imgset_template[CR_FD_PAGES_COMP].fmt, 1);
	comp_fd = openat(dfd, image_path, O_RDONLY, CR_FD_PERM);
	if (0 > comp_fd) {
		pr_warn("Can't open '%s' for dfd=%d, errno=%d\n", image_path, dfd, errno);
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
			close(comp_fd);
			return -1;
		}
		ret = pthread_create(&decomp_thread, NULL, decompression_thread_routine, (void *)(intptr_t)comp_fd);
		if (sigprocmask(SIG_SETMASK, &oldmask, NULL) == -1) {
			pr_perror("Can not unset mask of blocked signals");
		}
		if (ret) {
			pr_err("Can't start decompression thread, pthread_create returned %d\n", ret);
			close(comp_fd);
			return ret;
		}
	}
	pr_debug("Decompression thread started\n");
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
	ret = open(temp_path_decompressed, O_RDONLY, S_IRUSR | S_IWUSR);
	if (0 > ret) {
		pr_err("Failed open %s errno=%d\n", temp_path_decompressed, errno);
	}
	return ret;
}
