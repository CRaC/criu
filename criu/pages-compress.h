#ifndef __CR_PAGES_COMPRESS_H__
#define __CR_PAGES_COMPRESS_H__

#include "common/lock.h"

int compress_images(void);

void decompression_mutex_init(mutex_t *mtx);
int decompression_thread_start(void);
int decompression_get_fd(void);

#endif
