#include <unistd.h>
#include "compression.h"
#include "log.h"
#include "memfd.h"

#include <lz4/programs/lz4io.h>
#include <lz4/lib/lz4frame.h>


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

	pr_info("XXX %s:%d: %s: path=%s, ret=%d\n", __FILE__, __LINE__, __FUNCTION__, path, ret);
	return ret;
}
