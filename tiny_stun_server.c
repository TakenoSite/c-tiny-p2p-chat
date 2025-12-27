#define _POSIX_C_SOURCE 200112L
#include "tiny_stun_server.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#ifndef NI_MAXHOST
#define NI_MAXHOST 1025
#endif
#ifndef NI_MAXSERV
#define NI_MAXSERV 32
#endif

#define NTS_KEEPALIVE_INTERVAL_SEC 15
#define NTS_KEEPALIVE_PAYLOAD "KEEPALIVE"

/* 安全にaddrを文字列へ変換（失敗時は"?"と"0"を入れる） */
static void addr_to_str(const struct sockaddr_storage *addr, socklen_t addrlen, char *host, size_t hostlen, char *serv, size_t servlen) {
    if (!addr) return;
    int rc = getnameinfo((const struct sockaddr *)addr, addrlen, host, hostlen, serv, servlen, NI_NUMERICHOST | NI_NUMERICSERV);
    if (rc != 0) {
        snprintf(host, hostlen, "?");
        snprintf(serv, servlen, "0");
    }
}

struct nts_worker_arg {
    int sock;
    struct nts_ctx *table;
    size_t data_len;
    struct sockaddr_storage src;
    socklen_t srclen;
    char data[];
};

static void *nts_worker(void *p) {
    struct nts_worker_arg *w = (struct nts_worker_arg *)p;
    const size_t min_register = sizeof(uint32_t);
    const size_t min_query = sizeof(uint32_t) * 2;

    /* 問い合わせパケット: 先頭8バイト (要求者ID, 対象ID) を読み取り、対象の接続情報を返す */
    if (w->data_len >= min_query) {
        uint32_t net_req_id = 0;
        uint32_t net_target_id = 0;
        memcpy(&net_req_id, w->data, sizeof(uint32_t));
        memcpy(&net_target_id, w->data + sizeof(uint32_t), sizeof(uint32_t));

        uint32_t target_id = ntohl(net_target_id);
        char target_str[32];
        snprintf(target_str, sizeof(target_str), "%u", target_id);

        char host[NI_MAXHOST];
        char serv[NI_MAXSERV];
        addr_to_str(&w->src, w->srclen, host, sizeof(host), serv, sizeof(serv));

        printf("server <- query req_id=%u target_id=%u from %s:%s (%zu bytes)\n", ntohl(net_req_id), target_id, host, serv, w->data_len);

        struct client_info *peer = nts_find_client(w->table, target_str);
        char resp[128];
        int resp_len = 0;
        if (peer) {
            /* --- 対象が見つかった場合: 要求元へ応答し、同時に対象(peer)へ通知を送る --- */
            resp_len = snprintf(resp, sizeof(resp), "PEER %s %u\n", peer->ip, peer->port);

            /* 対象(peer)へ、要求者のグローバルIP/ポートとIDを通知してパンチ開始を促す */
            char notify[128];
            int nlen = snprintf(notify, sizeof(notify), "PUNCH %s %s %u\n", host, serv, ntohl(net_req_id));
            char portstr[16];
            snprintf(portstr, sizeof(portstr), "%u", peer->port);

            struct addrinfo hints = {0};
            struct addrinfo *ai = NULL;
            hints.ai_family = AF_UNSPEC;
            hints.ai_socktype = SOCK_DGRAM;
            hints.ai_flags = AI_NUMERICHOST | AI_NUMERICSERV;

            if (getaddrinfo(peer->ip, portstr, &hints, &ai) == 0 && ai) {
                sendto(w->sock, notify, (size_t)nlen, 0, ai->ai_addr, ai->ai_addrlen);
                printf("server -> notify target_id=%u (%s:%s) to punch req_id=%u at %s:%s '%.*s'\n",
                       target_id, peer->ip, portstr, ntohl(net_req_id), host, serv, nlen, notify);
                freeaddrinfo(ai);
            }
        } else {
            /* --- 見つからない場合: NOTFOUNDを返信 --- */
            resp_len = snprintf(resp, sizeof(resp), "NOTFOUND\n");
        }

        sendto(w->sock, resp, (size_t)resp_len, 0, (struct sockaddr *)&w->src, w->srclen);
        printf("server -> query resp to %s:%s '%.*s' (%d bytes)\n", host, serv, resp_len, resp, resp_len);
        (void)net_req_id; /* 未使用警告回避 */
    }

    /* 登録パケット: 先頭4バイト (クライアントID) を読み取り、送信元の外向きIP/ポートをテーブルへ保存 */
    else if (w->data_len >= min_register) {
        uint32_t net_id = 0;
        memcpy(&net_id, w->data, sizeof(uint32_t));
        uint32_t id = ntohl(net_id);

        char host[NI_MAXHOST];
        char serv[NI_MAXSERV];
        addr_to_str(&w->src, w->srclen, host, sizeof(host), serv, sizeof(serv));
        uint16_t port_host = (uint16_t)atoi(serv);

        char id_str[32];
        snprintf(id_str, sizeof(id_str), "%u", id);
        nts_add_client(w->table, id_str, host, port_host);
        printf("server <- register id=%u from %s:%s (%zu bytes)\n", id, host, serv, w->data_len);
		
        /* --- テーブル登録完了: TABLE_REGISTER を返信 --- */
        char ack[128];
        int ack_len = snprintf(ack, sizeof(ack), "TABLE_REGISTER %u\n", id);
        if (ack_len > 0) {
            sendto(w->sock, ack, (size_t)ack_len, 0,
                   (struct sockaddr *)&w->src, w->srclen);
            printf("server -> register ack to %s:%s '%.*s' (%d bytes)\n",
                   host, serv, ack_len, ack, ack_len);
        }
    }

    /* 先頭4バイトすら無いパケットは無視する */

    free(w);
    return NULL;
}

/* keep-alive送信用スレッドに渡すパラメータ */
struct nts_keepalive_arg {
    int sock;             /* 送信に使うサーバソケット */
    struct nts_ctx *table;/* 登録済みクライアントテーブル（ロック済みでアクセス） */
};

/*
 * keep-aliveループ:
 *   一定間隔ごと(NTS_KEEPALIVE_INTERVAL_SEC)に、テーブル内の全クライアントへ
 *   "KEEPALIVE" をUDPで送信し、NATマッピングの継続を狙う。
 */
static void *nts_keepalive_loop(void *p) {
    struct nts_keepalive_arg *ka = (struct nts_keepalive_arg *)p;
    const char payload[] = NTS_KEEPALIVE_PAYLOAD;
    struct timespec ts = {.tv_sec = NTS_KEEPALIVE_INTERVAL_SEC, .tv_nsec = 0};

    for (;;) {
        /* 送信間隔待ち */
        nanosleep(&ts, NULL);

        /* テーブルをロックして全エントリを走査し、各peerへ送信 */
        pthread_mutex_lock(&ka->table->lock);
        for (struct client_node *n = ka->table->head; n; n = n->next) {
            char portstr[16];
            snprintf(portstr, sizeof(portstr), "%u", n->info.port);

            struct addrinfo hints = {0};
            struct addrinfo *ai = NULL;
            hints.ai_family = AF_UNSPEC;       /* IPv4/IPv6どちらでも解決 */
            hints.ai_socktype = SOCK_DGRAM;
            hints.ai_flags = AI_NUMERICHOST | AI_NUMERICSERV;

            if (getaddrinfo(n->info.ip, portstr, &hints, &ai) == 0 && ai) {
                sendto(ka->sock, payload, sizeof(payload), 0, ai->ai_addr, ai->ai_addrlen);
                freeaddrinfo(ai);
            }
        }
        pthread_mutex_unlock(&ka->table->lock);
    }

    return NULL;
}

/*
 * 1パケット受信し、Full Cone NAT 前提で送信元グローバルIP/ポートを取得して登録する。
 * フォーマット: 先頭4バイトがクライアントID (network byte orderのuint32_t)、以降は任意ペイロード。
 * buf_pool: 受信バッファを確保するためのメモリプール。
 */
int nts_server_handle_once(int sock, struct nts_ctx *table, struct mm_pool *buf_pool, size_t buf_size) {
    if (!table || !buf_pool || buf_size < sizeof(uint32_t)) return -1;

    /* 受信用バッファをプールから取得 */
    void *buf = mm_pool_alloc(buf_pool);
    if (!buf) return -1;

    struct sockaddr_storage src;
    socklen_t srclen = sizeof(src);

    /* UDPを1パケット受信。Full Cone NATでは送信元IP/ポートがそのまま外向き公開情報 */
    ssize_t n = recvfrom(sock, buf, buf_size, 0, (struct sockaddr *)&src, &srclen);
    if (n < (ssize_t)sizeof(uint32_t)) {
        mm_pool_free(buf_pool, buf);
        return -1;
    }

    /* 先頭4バイトがクライアントID (network byte order) */
    uint32_t net_id;
    memcpy(&net_id, buf, sizeof(uint32_t));
    uint32_t id = ntohl(net_id);

    char id_str[32];
    snprintf(id_str, sizeof(id_str), "%u", id);

    /* 送信元アドレスを文字列化し、ポートは数値化する */
    char host[NI_MAXHOST];
    char serv[NI_MAXSERV];
    addr_to_str(&src, srclen, host, sizeof(host), serv, sizeof(serv));

    uint16_t port_host = (uint16_t)atoi(serv);

    /* テーブルへ登録（既存なら上書き） */
    int rc = nts_add_client(table, id_str, host, port_host);

    mm_pool_free(buf_pool, buf);
    return rc;
}

/*
 * クライアントからの問い合わせ(要求者ID, 対象IDの2つのuint32_t)を受け付け、
 * 対象のIP/ポートを文字列で応答する。Full Cone NAT では受信元が見えていれば十分。
 */
int nts_server_handle_query_once(int sock, struct nts_ctx *table, struct mm_pool *buf_pool, size_t buf_size) {
    if (!table || !buf_pool || buf_size < sizeof(uint32_t) * 2) return -1;

    void *buf = mm_pool_alloc(buf_pool);
    if (!buf) return -1;

    struct sockaddr_storage src;
    socklen_t srclen = sizeof(src);
    ssize_t n = recvfrom(sock, buf, buf_size, 0, (struct sockaddr *)&src, &srclen);
    if (n < (ssize_t)(sizeof(uint32_t) * 2)) {
        mm_pool_free(buf_pool, buf);
        return -1;
    }

    /* 先頭: 要求者ID、次: 対象ID (ともに network byte order) */
    uint32_t net_req;
    uint32_t net_target;
    memcpy(&net_req, buf, sizeof(uint32_t));
    memcpy(&net_target, (char *)buf + sizeof(uint32_t), sizeof(uint32_t));
    uint32_t req_id = ntohl(net_req);
    uint32_t target_id = ntohl(net_target);

    char target_str[32];
    snprintf(target_str, sizeof(target_str), "%u", target_id);

    struct client_info *peer = nts_find_client(table, target_str);

    char resp[128];
    int resp_len = 0;
    if (peer) {
        resp_len = snprintf(resp, sizeof(resp), "PEER %s %u\n", peer->ip, peer->port);
    } else {
        resp_len = snprintf(resp, sizeof(resp), "NOTFOUND\n");
    }

    sendto(sock, resp, (size_t)resp_len, 0, (struct sockaddr *)&src, srclen);

    /* ログ用に要求者IDを未使用警告なく利用 (不要ならキャストのみ) */
    (void)req_id;

    mm_pool_free(buf_pool, buf);
    return 0;
}

int nts_server_run(int port, struct nts_ctx *table, struct mm_pool *buf_pool, size_t buf_size) {
    if (!table || !buf_pool || buf_size < sizeof(uint32_t)) return -1;

    FILE *logf = stdout; /* ログは端末へ出力 */
    setvbuf(logf, NULL, _IONBF, 0);

    /* IPv4で待ち受け（グローバルIP 27.127.28.2 宛のパケットを受ける想定） */
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return -1;

    struct sockaddr_in addr4;
    memset(&addr4, 0, sizeof(addr4));
    addr4.sin_family = AF_INET;
    addr4.sin_addr.s_addr = htonl(INADDR_ANY); /* 0.0.0.0:port でバインド */
    addr4.sin_port = htons((uint16_t)port);
    if (bind(sock, (struct sockaddr *)&addr4, sizeof(addr4)) != 0) {
        close(sock);
        return -1;
    }

    /* keep-alive送信スレッドを起動 */
    struct nts_keepalive_arg ka = {
        .sock = sock,
        .table = table,
    };
    pthread_t ka_th;
    if (pthread_create(&ka_th, NULL, nts_keepalive_loop, &ka) == 0) {
        pthread_detach(ka_th);
    }

    for (;;) {
        void *buf = mm_pool_alloc(buf_pool);
        if (!buf) return -1;

        struct sockaddr_storage src;
        socklen_t srclen = sizeof(src);
        ssize_t n = recvfrom(sock, buf, buf_size, 0, (struct sockaddr *)&src, &srclen);
        if (n < 0) {
            mm_pool_free(buf_pool, buf);
            return -1;
        }

        /* 受信データをログ出力（送信元とバイト数） */
        char host[NI_MAXHOST];
        char serv[NI_MAXSERV];
        addr_to_str(&src, srclen, host, sizeof(host), serv, sizeof(serv));
        fprintf(logf, "server <- pkt from %s:%s (%zd bytes)\n", host, serv, n);

        size_t wsize = sizeof(struct nts_worker_arg) + (size_t)n;
        struct nts_worker_arg *w = (struct nts_worker_arg *)malloc(wsize);
        if (!w) {
            mm_pool_free(buf_pool, buf);
            return -1;
        }
        w->sock = sock;
        w->table = table;
        w->data_len = (size_t)n;
        w->src = src;
        w->srclen = srclen;
        memcpy(w->data, buf, (size_t)n);

        mm_pool_free(buf_pool, buf);

        pthread_t th;
        if (pthread_create(&th, NULL, nts_worker, w) == 0) {
            pthread_detach(th);
        } else {
            free(w);
            return -1;
        }
    }
}
