CFLAGS ?= -O2 -Wall -Wextra -Wno-sign-compare
test_bw_spd: test_bw_spd.c ../../common/c2c_proto.h
	$(CC) $(CFLAGS) -I../../common -o $@ $< -lpthread
clean:
	rm -f test_bw_spd
