#ifndef __CR_PAGES_COMPRESS_H__
#define __CR_PAGES_COMPRESS_H__

int compress_images(void);

int decompression_thread_start(void);
int decompression_thread_join(void);
int decompression_get_fd(void);
int decompression_unlink_tmpfile(void);

#endif
