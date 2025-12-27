#define _POSIX_C_SOURCE 200112L
#include "tiny_peer_table.h"
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

/* 安全に文字列をコピーし終端するユーティリティ */
static void safe_copy(char *dst, size_t dst_size, const char *src) {
    if (!dst || dst_size == 0) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    strncpy(dst, src, dst_size - 1);
    dst[dst_size - 1] = '\0';
}

/* プールからノードを取得（確保失敗ならNULL） */
static struct client_node *alloc_node(struct nts_ctx *ctx) {
    return (struct client_node *)mm_pool_alloc(&ctx->pool);
}

/* ノードをプールに返却して再利用可能にする */
static void free_node(struct nts_ctx *ctx, struct client_node *node) {
    mm_pool_free(&ctx->pool, node);
}

/* コンテキスト初期化: 指定容量でメモリプールを構築 */
int nts_init(struct nts_ctx *ctx, size_t capacity) {
    if (!ctx || capacity == 0) return -1;
    memset(ctx, 0, sizeof(*ctx));
    if (mm_pool_init(&ctx->pool, sizeof(struct client_node), capacity) != 0) {
        return -1;
    }
    ctx->capacity = capacity;
    ctx->head = NULL;
    ctx->count = 0;
    if (pthread_mutex_init(&ctx->lock, NULL) != 0) {
        mm_pool_destroy(&ctx->pool);
        return -1;
    }
    return 0;
}

/* 全ノードを解放し、メモリプールを破棄 */
void nts_dispose(struct nts_ctx *ctx) {
    if (!ctx) return;
    pthread_mutex_lock(&ctx->lock);
    struct client_node *cur = ctx->head;
    while (cur) {
        struct client_node *next = cur->next;
        free_node(ctx, cur);
        cur = next;
    }
    ctx->head = NULL;
    ctx->count = 0;
    pthread_mutex_unlock(&ctx->lock);
    pthread_mutex_destroy(&ctx->lock);
    mm_pool_destroy(&ctx->pool);
}

/* IDでノードを検索し、必要なら前ノードも返す（線形探索） */
static struct client_node *find_node(struct nts_ctx *ctx, const char *id, struct client_node **prev_out) {
    struct client_node *prev = NULL;
    struct client_node *cur = ctx->head;
    while (cur) {
        if (strncmp(cur->info.id, id, NTS_ID_MAX) == 0) {
            if (prev_out) *prev_out = prev;
            return cur;
        }
        prev = cur;
        cur = cur->next;
    }
    if (prev_out) *prev_out = NULL;
    return NULL;
}

/* 追加/更新: 既存IDなら上書き、空きがあれば新規挿入 */
int nts_add_client(struct nts_ctx *ctx, const char *id, const char *ip, uint16_t port) {
    if (!ctx || !id || !ip) return -1;

    pthread_mutex_lock(&ctx->lock);

    struct client_node *prev = NULL;
    struct client_node *existing = find_node(ctx, id, &prev);
    if (existing) {
        safe_copy(existing->info.ip, sizeof(existing->info.ip), ip);
        existing->info.port = port;
        pthread_mutex_unlock(&ctx->lock);
        return 0;
    }

    if (ctx->count >= ctx->capacity) {
        pthread_mutex_unlock(&ctx->lock);
        return -1;
    }

    struct client_node *node = alloc_node(ctx);
    if (!node) {
        pthread_mutex_unlock(&ctx->lock);
        return -1;
    }

    safe_copy(node->info.id, sizeof(node->info.id), id);
    safe_copy(node->info.ip, sizeof(node->info.ip), ip);
    node->info.port = port;

    node->next = ctx->head;
    ctx->head = node;
    ctx->count += 1;
    pthread_mutex_unlock(&ctx->lock);
    return 0;
}

/* 削除: リストから外しプールに返却 */
int nts_remove_client(struct nts_ctx *ctx, const char *id) {
    if (!ctx || !id) return -1;
    pthread_mutex_lock(&ctx->lock);
    struct client_node *prev = NULL;
    struct client_node *node = find_node(ctx, id, &prev);
    if (!node) {
        pthread_mutex_unlock(&ctx->lock);
        return -1;
    }

    if (prev) {
        prev->next = node->next;
    } else {
        ctx->head = node->next;
    }
    free_node(ctx, node);
    ctx->count -= 1;
    pthread_mutex_unlock(&ctx->lock);
    return 0;
}

/* 検索: ヒット時はクライアント情報へのポインタを返す */
struct client_info *nts_find_client(struct nts_ctx *ctx, const char *id) {
    if (!ctx || !id) return NULL;
    pthread_mutex_lock(&ctx->lock);
    struct client_node *node = find_node(ctx, id, NULL);
    struct client_info *res = node ? &node->info : NULL;
    pthread_mutex_unlock(&ctx->lock);
    return res;
}

/* 現在の登録数を返す */
size_t nts_count(const struct nts_ctx *ctx) {
    if (!ctx) return 0;
    pthread_mutex_lock((pthread_mutex_t *)&ctx->lock);
    size_t c = ctx->count;
    pthread_mutex_unlock((pthread_mutex_t *)&ctx->lock);
    return c;
}
