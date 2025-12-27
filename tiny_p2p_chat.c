#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <time.h>
#include <unistd.h>

#ifndef NI_MAXHOST
#define NI_MAXHOST 1025
#endif
#ifndef NI_MAXSERV
#define NI_MAXSERV 32
#endif

#define PUNCH_COUNT  10
#define PUNCH_INTERVAL_NS (30 * 1000 * 1000)
#define BUF_SIZE 512

static void die(const char *msg)
{
    perror(msg);
    exit(EXIT_FAILURE);
}

static void sleep_ns(long ns)
{
    struct timespec ts = {0, ns};
    nanosleep(&ts, NULL);
}

static int udp_socket(void)
{
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) die("socket");
    return s;
}

/* ---------- サーバ ---------- */

static void server_addr(struct sockaddr_in *srv, const char *host, uint16_t port)
{
    memset(srv, 0, sizeof(*srv));
    srv->sin_family = AF_INET;
    srv->sin_port   = htons(port);
    inet_pton(AF_INET, host, &srv->sin_addr);
}

static void register_self(int sock, uint32_t self_id, const char *host, uint16_t port)
{
    uint32_t id = htonl(self_id);
    struct sockaddr_in srv;
    server_addr(&srv, host, port);

    if (sendto(sock, &id, sizeof(id), 0,
               (struct sockaddr *)&srv, sizeof(srv)) != sizeof(id))
        die("register");
}

static int query_peer(int sock, uint32_t self_id, uint32_t peer_id,
                      char *ip, unsigned *port,
                      const char *host, uint16_t server_port)
{
    uint32_t q[2] = { htonl(self_id), htonl(peer_id) };
    char resp[128];
    struct sockaddr_storage src;
    socklen_t slen = sizeof(src);

    struct sockaddr_in srv;
    server_addr(&srv, host, server_port);

    sendto(sock, q, sizeof(q), 0, (struct sockaddr *)&srv, sizeof(srv));

    ssize_t r = recvfrom(sock, resp, sizeof(resp) - 1, 0,
                         (struct sockaddr *)&src, &slen);
    if (r <= 0) return -1;

    resp[r] = '\0';
    char tag[16];
    if (sscanf(resp, "%15s %63s %u", tag, ip, port) == 3 &&
        strcmp(tag, "PEER") == 0)
        return 0;

    return -1;
}

/* ---------- P2P ---------- */

static void punch_peer(int sock,
                       const struct sockaddr *peer, socklen_t peerlen)
{
    for (int i = 0; i < PUNCH_COUNT; i++) {
        sendto(sock, NULL, 0, 0, peer, peerlen);
        sleep_ns(PUNCH_INTERVAL_NS);
    }
}

static int make_peer_addr(const char *ip, unsigned port,
                          struct sockaddr_storage *out, socklen_t *outlen)
{
    struct addrinfo hints = {0}, *ai = NULL;
    char portstr[16];
    snprintf(portstr, sizeof(portstr), "%u", port);

    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags    = AI_NUMERICHOST | AI_NUMERICSERV;

    if (getaddrinfo(ip, portstr, &hints, &ai) != 0)
        return -1;

    memcpy(out, ai->ai_addr, ai->ai_addrlen);
    *outlen = ai->ai_addrlen;
    freeaddrinfo(ai);
    return 0;
}

/* ---------- main ---------- */

int main(int argc, char **argv)
{
    if (argc != 6) {
        fprintf(stderr, "usage: %s <self_id> <peer_id> <server_host> <server_port> <-r|-c>\n", argv[0]);
        return 1;
    }

    uint32_t self_id = atoi(argv[1]);
    uint32_t peer_id = atoi(argv[2]);
    const char *cli_host = argv[3];

    char *end = NULL;
    errno = 0;
    unsigned long cli_port = strtoul(argv[4], &end, 10);
    if (errno != 0 || end == argv[4] || *end != '\0' || cli_port == 0 || cli_port > UINT16_MAX) {
        fprintf(stderr, "invalid server_port\n");
        return 1;
    }

    int is_receiver = strcmp(argv[5], "-r") == 0;
    int is_client   = strcmp(argv[5], "-c") == 0;

    if (!is_receiver && !is_client) {
        fprintf(stderr, "invalid mode\n");
        return 1;
    }

    uint16_t server_port = (uint16_t)cli_port;
    char server_host[NI_MAXHOST];
    strncpy(server_host, cli_host, sizeof(server_host) - 1);
    server_host[sizeof(server_host) - 1] = '\0';

    int sock = udp_socket();

    /* 1. register */
    register_self(sock, self_id, server_host, server_port);
    printf("registered id=%u\n", self_id);

    struct sockaddr_storage peer_addr;
    socklen_t peer_len = 0;
    int peer_ready = 0;

    /* ========== -r : 受信待機ノード ========== */
    if (is_receiver) {
        printf("receiver mode. waiting server notify...\n");

        for (;;) {
            char buf[BUF_SIZE];
            struct sockaddr_storage src;
            socklen_t slen = sizeof(src);

            ssize_t r = recvfrom(sock, buf, sizeof(buf) - 1, 0,
                                 (struct sockaddr *)&src, &slen);
            if (r <= 0)
                continue;

            buf[r] = '\0';

            /* サーバ通知: PUNCH ip port peer_id */
            char tag[16], ip[64];
            unsigned port, rid;

            if (sscanf(buf, "%15s %63s %u %u", tag, ip, &port, &rid) == 4 &&
                strcmp(tag, "PUNCH") == 0) {

                printf("server notify: peer=%u %s:%u\n", rid, ip, port);

                if (make_peer_addr(ip, port, &peer_addr, &peer_len) == 0) {
                    peer_ready = 1;
                    punch_peer(sock,
                               (struct sockaddr *)&peer_addr,
                               peer_len);
                    printf("punch sent to peer\n");
                    break;
                }
            }
        }
    }

    /* ========== -c : 探索ノード ========== */
    if (is_client) {
        char peer_ip[64];
        unsigned peer_port = 0;

        printf("client mode. querying peer...\n");

        for (int i = 0; i < 20; i++) {
            if (query_peer(sock, self_id, peer_id,
                           peer_ip, &peer_port,
                           server_host, server_port) == 0)
                break;
            sleep_ns(100 * 1000 * 1000);
        }

        printf("peer resolved: %s:%u\n", peer_ip, peer_port);

        if (make_peer_addr(peer_ip, peer_port,
                           &peer_addr, &peer_len) != 0)
            die("peer addr");

        peer_ready = 1;

        punch_peer(sock,
                   (struct sockaddr *)&peer_addr,
                   peer_len);
        printf("punch sent to peer\n");
    }

    if (!peer_ready) {
        fprintf(stderr, "peer address not resolved.\n");
        close(sock);
        return 1;
    }

    /* ========== 共通: チャット ========== */
    printf("p2p established. start chat.\n> ");
    fflush(stdout);

    for (;;) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(sock, &rfds);
        FD_SET(STDIN_FILENO, &rfds);

        select(sock + 1, &rfds, NULL, NULL, NULL);

        /* 受信処理 */
        if (FD_ISSET(sock, &rfds)) {
            char buf[BUF_SIZE];
            ssize_t n = recvfrom(sock, buf, sizeof(buf) - 1, 0, NULL, NULL);
            if (n > 0) {
                buf[n] = '\0';
                printf("\n[peer] %s\n> ", buf);
                fflush(stdout);
            }
        }
        /* 送信処理 */
        if (FD_ISSET(STDIN_FILENO, &rfds)) {
            char buf[BUF_SIZE];
            if (!fgets(buf, sizeof(buf), stdin))
                break;

            sendto(sock, buf, strlen(buf), 0,
                   (struct sockaddr *)&peer_addr, peer_len);

            char host[NI_MAXHOST];
            char serv[NI_MAXSERV];

            if (getnameinfo((struct sockaddr *)&peer_addr, peer_len,
                            host, sizeof(host), serv, sizeof(serv),
                            NI_NUMERICHOST | NI_NUMERICSERV) == 0) {
                printf("[send -> %s:%s]\n", host, serv);
            }

            printf("> ");
            fflush(stdout);
        }
    }

    close(sock);
    return 0;
}
