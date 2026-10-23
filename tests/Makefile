# Host tests for the virtual network shim.
#
# The shim has no Horizon dependency on purpose, so it can be built and run
# with an ordinary compiler long before a devkitPro toolchain is involved.
#
#   make          build and run
#   make cross    additionally compile for aarch64 to catch alignment and
#                 codegen problems the host build would hide

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O1 -g -Wall -Wextra -Wshadow -Wconversion -Wno-unused-parameter \
            -fno-exceptions -fno-rtti $(SANITIZE)

SRC := test_vnet.cpp ../source/net/vnet.cpp

# Sanitizers: en algunos hosts (p. ej. x86_64 bajo Rosetta) los binarios con
# ASan/UBSan abortan al arrancar, también con el test original. `make SANITIZE=`
# los desactiva.
SANITIZE ?= -fsanitize=address,undefined -fno-omit-frame-pointer

LWIP_INC := -I../lib/lwip/src/include -I../lib/lwip-port
TCP_SRC  := test_tcp.cpp ../source/net/vnet.cpp ../source/net/tcp_stack.cpp ../source/mitm/tcp_sockets.cpp

.PHONY: all run tcp policy cross clean

all: run

test_vnet: $(SRC) ../source/net/vnet.hpp ../source/net/net.hpp ../source/net/lifetime.hpp
	$(CXX) $(CXXFLAGS) -o $@ $(SRC)

test_nat_support: test_nat_support.c ../compat/nat/support.c
	$(CC) -std=gnu11 -O1 -g -Wall -Wextra $(SANITIZE) -o $@ $^

lwip_unity.o: ../source/net/lwip_unity.c ../lib/lwip-port/lwipopts.h
	$(CC) -std=gnu11 -O1 -g -w $(LWIP_INC) -c -o $@ $<

test_tcp: $(TCP_SRC) lwip_unity.o ../source/net/tcp_stack.hpp ../source/net/vnet.hpp ../source/mitm/tcp_sockets.hpp
	$(CXX) $(CXXFLAGS) $(LWIP_INC) -o $@ $(TCP_SRC) lwip_unity.o

test_policy: test_policy.cpp ../source/mitm/mitm_policy.hpp
	$(CXX) $(CXXFLAGS) -o $@ test_policy.cpp

run: test_vnet test_nat_support test_tcp test_policy test_perf_new
	./test_vnet
	./test_nat_support
	./test_tcp
	./test_policy
	./test_perf_new fast

tcp: test_tcp
	./test_tcp

policy: test_policy
	./test_policy

# The console is aarch64. Unaligned loads and sign-extension differences are the
# usual surprises; compiling with the cross toolchain catches most of them even
# without a way to execute the result.
cross:
	aarch64-linux-gnu-g++ -std=c++17 -O2 -Wall -Wextra -Wconversion \
		-fno-exceptions -fno-rtti -c ../source/net/vnet.cpp -o /tmp/vnet-aarch64.o
	@echo "aarch64 build OK"

clean:
	rm -f test_vnet test_nat_support test_tcp test_policy test_perf_base test_perf_new lwip_unity.o lwip_unity_base.o
	rm -rf test_vnet.dSYM

# Rendimiento TCP simulado: baseline (opciones lwIP anteriores, loop de 10 ms) frente a la configuración actual.
PERF_SRC := test_perf.cpp ../source/net/vnet.cpp ../source/net/tcp_stack.cpp

lwip_unity_base.o: ../source/net/lwip_unity.c ../lib/lwip-port/lwipopts.h
	$(CC) -std=gnu11 -O1 -g -w -DZTNX_PERF_BASELINE $(LWIP_INC) -c -o $@ $<

test_perf_base: $(PERF_SRC) lwip_unity_base.o ../lib/lwip-port/lwipopts.h
	$(CXX) $(CXXFLAGS) -DZTNX_PERF_BASELINE $(LWIP_INC) -o $@ $(PERF_SRC) lwip_unity_base.o

test_perf_new: $(PERF_SRC) lwip_unity.o ../lib/lwip-port/lwipopts.h
	$(CXX) $(CXXFLAGS) $(LWIP_INC) -o $@ $(PERF_SRC) lwip_unity.o

perf: test_perf_base test_perf_new
	@echo "== antes (lwIP anterior, nodo 10 ms, app 5 ms) =="; ./test_perf_base
	@echo "== solo lwIP nuevo (nodo 10 ms, app 5 ms) =="; ./test_perf_new
	@echo "== después (lwIP nuevo, nodo 2 ms, app 1 ms) =="; ./test_perf_new fast
