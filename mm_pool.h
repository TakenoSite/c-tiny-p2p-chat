#ifndef MM_POOL_H
#define MM_POOL_H

#include <stddef.h>

/* 固定長要素を高速に確保/解放するシンプルなメモリプール */
struct mm_pool {
    void *mem;               /* 生メモリブロック */
    size_t elem_size;        /* 1要素のサイズ */
    size_t capacity;         /* 最大要素数 */
    void *free_list;         /* 単方向リストで空き要素を管理（スロット先頭にnextを格納） */
};

int mm_pool_init(struct mm_pool *pool, size_t elem_size, size_t capacity);
void mm_pool_destroy(struct mm_pool *pool);
void *mm_pool_alloc(struct mm_pool *pool);
void mm_pool_free(struct mm_pool *pool, void *ptr);

#endif
