#ifndef TINY_STUN_SERVER_H
#define TINY_STUN_SERVER_H

#include <stddef.h>
#include "tiny_peer_table.h"
#include "mm_pool.h"

/* 単発でUDPパケットを受信し、クライアント情報をテーブルに反映する */
int nts_server_handle_once(int sock, struct nts_ctx *table, struct mm_pool *buf_pool, size_t buf_size);

/*
 * クライアントからの問い合わせ(要求者ID, 対象IDの2つのuint32_t, network byte order)を受け付け、
 * 対象クライアント(bob等)のIP/ポートを文字列でレスポンスする。
 * 見つかった場合: "PEER <ip> <port>\n"
 * 見つからない場合: "NOTFOUND\n"
 */
int nts_server_handle_query_once(int sock, struct nts_ctx *table, struct mm_pool *buf_pool, size_t buf_size);

/*
 * シンプルなイベントループ。portで指定したUDPポートを::でlistenし、
 * 受信パケットの長さで登録/問い合わせを判定して処理する。
 * エラーで-1。ループは終了しない設計なので、呼び出し側でプロセス終了を管理する。
 */
int nts_server_run(int port, struct nts_ctx *table, struct mm_pool *buf_pool, size_t buf_size);

#endif
