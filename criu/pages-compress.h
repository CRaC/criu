#ifndef __CR_PAGES_COMPRESS_H__
#define __CR_PAGES_COMPRESS_H__

int compress_images(void);
int decompress_image(int comp_fd, const char *path);

#endif