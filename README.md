# P2P NAT Traversal Sample (C)

## 概要
- 本リポジトリはC言語で実装された、UDPベースのシンプルなP2P NAT traversalサンプルです。Full Cone NATを前提に、サーバがクライアントの外向きIP/PORTを登録し、相手peer情報を通知します。クライアントはhole punch後に直接UDP chatします。

## ビルド方法
```
make
```
実行ファイル:
- `tiny_stun_server_run` : STUN風のシンプルなUDPサーバ実行バイナリ
- `tiny_p2p_chat` : サーバ経由でピア解決しチャットするクライアント

## tiny_stun_server_run.c について
- UDPで待ち受け、クライアント登録とpeer問い合わせに応答します。
- 問い合わせを受けると、対象peerへ `PUNCH` 通知を送り、要求元には対象の外向きIP/PORTを `PEER <ip> <port>` で返します。
- keep-aliveを定期送信し、NAT mappingを維持します。

起動例:
```
./tiny_stun_server_run 45020
```
引数が無い場合はデフォルトでポート12345を使用します。

## tiny_p2p_chat.c について
- サーバに自分のIDを登録し、指定した相手IDの外向きアドレスを問い合わせます。
- サーバ通知または問い合わせ結果を元に、相手へUDP hole punchingを行い、そのままUDP chatを実施します。

使い方:
```
./tiny_p2p_chat <self_id> <peer_id> <server_host> <server_port> <-r|-c>
```
- `self_id` / `peer_id`: 通し番号など任意のID（数値想定）
- `server_host` / `server_port`: 上記サーバのアドレス
- `-r`: 受信待機ノード（サーバからのPUNCH通知を待ち、届いたら相手へpunching）
- `-c`: 探索ノード（サーバへ相手を問い合わせ、得たアドレスへpunching）

実行例（同一サーバに接続する場合）:
```
# ターミナル1: サーバ
./tiny_stun_server_run 45020

# ターミナル2: 受信待機ノード（例: self=100, peer=200）
./tiny_p2p_chat 100 200 127.0.0.1 45020 -r

# ターミナル3: 探索ノード（例: self=200, peer=100）
./tiny_p2p_chat 200 100 127.0.0.1 45020 -c
```
`p2p established. start chat.` が表示されたら、標準入力から送信できます。送信時に宛先IP:PORTが表示されます。

## 主要設定
- KEEPALIVE送信間隔: 15秒 (`NTS_KEEPALIVE_INTERVAL_SEC`)
- 受信バッファ/プールサイズなどはコード中の定数を参照してください。

## 前提
- POSIX互換環境 (Linux想定)
- gcc + pthread利用

