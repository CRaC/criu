#include <limits.h>
#include <stdio.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/resource.h>
#include <semaphore.h>

#include "pages-compress.h"
#include "crtools.h"
#include "image.h"
#include "log.h"
#include "memfd.h"
#include "util.h"

#include <lz4/programs/lz4io.h>
#include <lz4/lib/lz4frame.h>

unsigned pages_image_max_id(void);

const char *temp_path_decompressed = "/tmp/decompressed";

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
	// LZ4IO_setNotificationLevel(100);
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

#if 0
static int compress_image(const char *pathsrc, const char *pathdst) {

    const int LZ4_Compression_Level = 8;
	LZ4IO_prefs_t *lz4_prefs = LZ4IO_defaultPreferences();
    LZ4IO_setBlockSize(lz4_prefs, 128 * 1024);// * 1024);
    LZ4IO_setNotificationLevel(100);

    if (LZ4IO_compressFilename(pathsrc, pathdst, LZ4_Compression_Level, lz4_prefs)) {
        LZ4IO_freePreferences(lz4_prefs);
        pr_err("Can't compress %s to %s\n", pathsrc, pathdst);
        return -1;
    }
	LZ4IO_freePreferences(lz4_prefs);
    return 0;
}

static int decompress_image_file(const char *pathsrc, const char *pathdst) {
	LZ4IO_prefs_t *lz4_prefs = LZ4IO_defaultPreferences();
    LZ4IO_setBlockSize(lz4_prefs, 128 * 1024);// * 1024);
    // LZ4IO_setNotificationLevel(100);

    if (LZ4IO_decompressFilename(pathsrc, pathdst, lz4_prefs)) {
        LZ4IO_freePreferences(lz4_prefs);
        pr_err("Can't compress %s to %s\n", pathsrc, pathdst);
        return -1;
    }
	LZ4IO_freePreferences(lz4_prefs);
    return 0;
}
#endif

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

	pr_debug("Creating file at '%s'\n", temp_path_decompressed);
	ret = open(temp_path_decompressed, O_CREAT | O_WRONLY, 0600);


	if (0 > ret) {
		pr_err("Can't create file, errno=%d\n", errno);
		LZ4F_freeDecompressionContext(dctx);
		return -1;
	}

	while (1) {
		size_t readbytes;
		readbytes = read(comp_fd, compbuf + offset, bytestoread);
		totalread += readbytes;
		if (!readbytes) {
			/* reached end of file or stream */
			break;
		}
		{
			size_t outsize = sizeof outbuf;
			size_t insize = offset + readbytes;
			const size_t insize_orig = insize;
			lz4err = LZ4F_decompress(dctx, outbuf, &outsize, compbuf, &insize, NULL);
			if (!LZ4F_isError(lz4err)) {
				{
					ssize_t res = write(ret, outbuf, outsize);
					if (0 > res) {
						pr_err("write error to output file, fd=%d, errno=%d\n", ret, errno);
						break;
					}
				}
				totalwrite += outsize;
				offset = insize_orig - insize;
				bytestoread = insize;
				memmove(compbuf, compbuf + compbufsize - offset, offset);
			} else {
				pr_err("LZ4 Decompress error: %s\n", LZ4F_getErrorName(lz4err));
				break;
			}
		}
	}
	pr_info("decompression completed, read %lu, wrote %lu\n", (unsigned long)totalread, (unsigned long)totalwrite);
	close(ret);
	return 0;
}

#define DECOMP_MUTEX_NAME "crac_decompression_mutex"
static sem_t *mutex_sem = NULL;

static pthread_t decomp_thread = 0;
static int decomp_fd = -1;

static void *decompression_thread_routine(void *param) {
	const int tid = syscall(SYS_gettid);
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
    } else {
	    decomp_fd = ret;
	}
	pr_debug("Decompression thread completed, pid=%d, tid=%d, decomp_fd=%d\n", getpid(), tid, decomp_fd);

	ret = sem_post(mutex_sem);
	if (-1 == ret) {
        pr_perror("sem_post failed"); 
	}
    return NULL;
}

int decompression_thread_start(void) {
    int ret;
    if (decomp_thread) {
        pr_err("Decompression thread already started\n");
        return -1;
    }

	pr_debug("Creating decompression mutex...\n");
	mutex_sem = sem_open(DECOMP_MUTEX_NAME, O_CREAT, 0660, 0);
    if (SEM_FAILED == mutex_sem) {
        pr_perror("sem_open failed"); 
		return -1;
    }

	pr_debug("Starting decompression thread...\n");
    ret = pthread_create(&decomp_thread, NULL, decompression_thread_routine, NULL);
    if (ret) {
        pr_err("Can't start decompression thread, pthread_create returned %d\n", ret);
        return ret;
    }

	pr_debug("Decompression thread started, decomp_thread=%lu\n", decomp_thread);
    return 0;
}

int decompression_thread_join(void) {
	int ret;
	pr_debug("wait for semaphore...\n");
	ret = sem_wait(mutex_sem);
	if (-1 == ret) {
        pr_perror("sem_wait failed"); 
		return -1;
	}
	if (0 <= decomp_fd) {
		pr_debug("Decompression FD ready, pid=%d, tid=%d, decomp_fd=%d\n", getpid(), (int)syscall(SYS_gettid), decomp_fd);
		return 0;
	}
	ret = sem_post(mutex_sem);
	if (-1 == ret) {
        pr_perror("sem_post failed"); 
		return -1;
    }
	pr_debug("Decompression thread joined, pid=%d, decomp_fd=%d\n", getpid(), decomp_fd);
    return 0;
}

int decompression_get_fd(void) {
	int ret;
    if (decompression_thread_join()) {
		return -1;
	}
	if (0 < decomp_fd) {
    	ret = dup(decomp_fd);
		if (0 > ret) {
			pr_err("Failed duplicate FD %d, errno=%d\n", decomp_fd, errno);
		}
		lseek(ret, 0, SEEK_SET);
	} else {
		ret = open(temp_path_decompressed, O_RDONLY, 0600);
		if (0 > ret) {
			pr_err("Failed open %s errno=%d\n", temp_path_decompressed, errno);
		}
	}
	return ret;
}
