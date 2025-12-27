CC ?= gcc
CFLAGS ?= -std=c11 -Wall -Wextra -O2
CFLAGS += -pthread
LDLIBS += -pthread

all: tiny_stun_server_run tiny_p2p_chat

TSSR_OBJS = tiny_stun_server_run.o tiny_stun_server.o tiny_peer_table.o mm_pool.o

# STUN-like server runner
tiny_stun_server_run: $(TSSR_OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(TSSR_OBJS) $(LDLIBS)

# Simple P2P chat client
tiny_p2p_chat: tiny_p2p_chat.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c $<

.PHONY: clean
clean:
	rm -f *.o tiny_stun_server_run tiny_p2p_chat
