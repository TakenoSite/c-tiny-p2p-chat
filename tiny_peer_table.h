#ifndef TINY_PEER_TABLE_H
#define TINY_PEER_TABLE_H

#include <stdint.h>
#include <stddef.h>
#include <pthread.h>
#include "mm_pool.h"

#define NTS_ID_MAX   63
#define NTS_IP_MAX   63

/* クライアント情報: IDとグローバルIP/ポートを保持 */
struct client_info {
    char id[NTS_ID_MAX + 1];
    char ip[NTS_IP_MAX + 1];
    uint16_t port; /* host byte order */
};

/* 単方向リストのノード（プール上に配置） */
struct client_node {
    struct client_info info;
    struct client_node *next;
};

/* サーバ側のクライアント管理コンテキスト */
struct nts_ctx {
    struct mm_pool pool;          /* ノード用メモリプール */
    struct client_node *head;     /* 先頭ノード */
    size_t capacity;              /* 最大クライアント数 */
    size_t count;                 /* 現在の登録数 */
    pthread_mutex_t lock;         /* スレッドセーフ用ロック */
};

int nts_init(struct nts_ctx *ctx, size_t capacity);
void nts_dispose(struct nts_ctx *ctx);
int nts_add_client(struct nts_ctx *ctx, const char *id, const char *ip, uint16_t port);
int nts_remove_client(struct nts_ctx *ctx, const char *id);
struct client_info *nts_find_client(struct nts_ctx *ctx, const char *id);
size_t nts_count(const struct nts_ctx *ctx);

#endif
