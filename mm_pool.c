#define _POSIX_C_SOURCE 200112L
#include "mm_pool.h"
#include <stdlib.h>
#include <string.h>

/* プール内部で利用するスロット。先頭にnextだけを持つ */
struct slot {
    struct slot *next;
};

int mm_pool_init(struct mm_pool *pool, size_t elem_size, size_t capacity) {
    /* 引数チェック: 不正なら失敗 */
    if (!pool || elem_size == 0 || capacity == 0) {
        return -1;
    }
    /* 要素サイズをスロット構造体以上にそろえ、メモリをまとめて確保 */
    pool->elem_size = elem_size < sizeof(struct slot) ? sizeof(struct slot) : elem_size;
    pool->capacity = capacity;
    pool->mem = calloc(capacity, pool->elem_size);
    if (!pool->mem) {
        return -1;
    }

    /* 確保したブロック上に空きスロットの単方向リストを構築 */
    char *p = (char *)pool->mem;
    pool->free_list = NULL;
    for (size_t i = 0; i < capacity; ++i) {
        struct slot *s = (struct slot *)(p + i * pool->elem_size);
        s->next = (struct slot *)pool->free_list;
        pool->free_list = s;
    }
    return 0;
}

void mm_pool_destroy(struct mm_pool *pool) {
    /* プール全体を破棄し、ポインタをクリア */
    if (!pool) return;
    free(pool->mem);
    pool->mem = NULL;
    pool->free_list = NULL;
    pool->capacity = 0;
    pool->elem_size = 0;
}

void *mm_pool_alloc(struct mm_pool *pool) {
    /* 空きがなければNULL */
    if (!pool || !pool->free_list) {
        return NULL;
    }
    /* 先頭の空きスロットを取り出す */
    struct slot *s = (struct slot *)pool->free_list;
    pool->free_list = s->next;
    /* 再利用時にゴミを残さないようゼロクリア */
    memset(s, 0, pool->elem_size);
    return s;
}

void mm_pool_free(struct mm_pool *pool, void *ptr) {
    /* 解放されたスロットをfree_list先頭へ戻す */
    if (!pool || !ptr) return;
    struct slot *s = (struct slot *)ptr;
    s->next = (struct slot *)pool->free_list;
    pool->free_list = s;
}
