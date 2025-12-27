#define _POSIX_C_SOURCE 200809L
#include "tiny_stun_server.h"
#include "mm_pool.h"
#include <errno.h>
#include <assert.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

static void on_alarm(int sig) {
    (void)sig;
    _exit(1); /* ハング防止 */
}

int main(int argc, char **argv) {
    uint16_t server_port = 12345;
    if (argc >= 2) {
        char *end = NULL;
        errno = 0;
        unsigned long port = strtoul(argv[1], &end, 10);
        if (errno != 0 || end == argv[1] || *end != '\0' || port == 0 || port > UINT16_MAX) {
            fprintf(stderr, "usage: %s [port]\n", argv[0]);
            return 1;
        }
        server_port = (uint16_t)port;
    }

    signal(SIGALRM, on_alarm);
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("server starting on %u (UDP)\n", server_port);

    struct nts_ctx table;
    assert(nts_init(&table, 16) == 0 && "init table");

    struct mm_pool bufpool;
    size_t buf_size = 1024;
    assert(mm_pool_init(&bufpool, buf_size, 8) == 0 && "init pool");

    /* サーバループ（戻らない設計）。SIGALRMで強制終了させる */
    (void)nts_server_run(server_port, &table, &bufpool, buf_size);

    mm_pool_destroy(&bufpool);
    nts_dispose(&table);
    return 0;
}
