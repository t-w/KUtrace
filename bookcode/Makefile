
#.SUFFIXES: .cpp

CXX = c++
CXXFLAGS = -Wall -Wextra -pedantic -O2

# get list of sources
SRC = $(wildcard *.cc)

# generate list of objects
OBJS = $(SRC:.cc=.o)

LIBS =

# and targets
#TARGETS = $(SRC:.cc=)
TARGETS = client4 \
    dumplogfile4 \
    eventtospan3 \
    matrix \
    flt_hog \
    hello_world_trace \
    kutrace_control \
    makeself \
    matrix \
    memhog_3 \
    memhog_ram \
    mystery0 \
    mystery1 \
    mystery2 \
    mystery3 \
    mystery23 \
    mystery25 \
    mystery27 \
    mystery27a \
    paging_hog \
    pcaptojson \
    queuetest \
    rawtoevent \
    samptoname_k \
    samptoname_u \
    schedtest \
    server4 \
    server_disk \
    server_mystery21 \
    spantospan \
    spantoprof \
    spantotrim \
    timealign \
    time_getpid \
    unmakeself \
    whetstone_ku



all: $(TARGETS)

$(TARGETS): %: %.o
	$(CXX) $< -o $@ $(LIBS)

client4: client4.o dclab_log.o dclab_rpc.o kutrace_lib.o
	$(CXX) $(CXXFLAGS) -pthread client4.cc dclab_log.cc dclab_rpc.cc kutrace_lib.cc \
             -o client4					
dumplogfile4: dumplogfile4.cc dclab_log.cc
	$(CXX) $(CXXFLAGS) dumplogfile4.cc dclab_log.cc -o dumplogfile4

#eventtospan3: eventtospan3.o

flt_hog: flt_hog.o kutrace_lib.o
	$(CXX) $(CXXFLAGS) $< kutrace_lib.o -o $@

hello_world_trace: hello_world_trace.o kutrace_lib.o
	$(CXX) $(CXXFLAGS) $< kutrace_lib.o -o $@

hello_world_trace.o: hello_world_trace.c
	$(CXX) -c $(CXXFLAGS) $< -o $@

kutrace_control: kutrace_control.o kutrace_lib.o
	$(CXX) $(CXXFLAGS) $< kutrace_lib.o -o $@

matrix: matrix.o kutrace_lib.o
	$(CXX) $(CXXFLAGS) $< kutrace_lib.o -o $@

memhog_3: memhog_3.o kutrace_lib.o
	$(CXX) $(CXXFLAGS) $< kutrace_lib.o -o $@

memhog_ram: memhog_ram.o kutrace_lib.o
	$(CXX) $(CXXFLAGS) $< kutrace_lib.o -o $@

mystery0: mystery0.o
mystery1: mystery1.o
mystery2: mystery2.o
mystery3: mystery3.o

mystery23: mystery23.o kutrace_lib.o
	$(CXX) $(CXXFLAGS) -pthread $< kutrace_lib.o -o $@

mystery25: mystery25.o kutrace_lib.o
	$(CXX) $(CXXFLAGS) $< kutrace_lib.o -o $@

mystery27: mystery27.o fancylock2.o mutex2.o kutrace_lib.o dclab_log.o
	$(CXX) $(CXXFLAGS) -pthread $< \
           fancylock2.o mutex2.o kutrace_lib.o dclab_log.o \
           -o $@

mystery27a: mystery27a.o fancylock2.o mutex2.o kutrace_lib.o dclab_log.o
	$(CXX) $(CXXFLAGS) -pthread $< \
           fancylock2.o mutex2.o kutrace_lib.o dclab_log.o \
           -o $@

paging_hog: paging_hog.o kutrace_lib.o
	$(CXX) $(CXXFLAGS) -pthread $< kutrace_lib.o -o $@

pcaptojson: pcaptojson.o dclab_log.o dclab_rpc.o kutrace_lib.o -lpcap
	$(CXX) $(CXXFLAGS) $< dclab_log.o dclab_rpc.o kutrace_lib.o \
           -lpcap -o $@

queuetest: queuetest.o dclab_log.o dclab_rpc.o kutrace_lib.o
	$(CXX) $(CXXFLAGS) -pthread $< dclab_log.o dclab_rpc.o kutrace_lib.o \
           -o $@

rawtoevent: rawtoevent.o from_base40.o kutrace_lib.o
	$(CXX) $(CXXFLAGS) $< from_base40.o kutrace_lib.o -o rawtoevent

samptoname_k: samptoname_k.o
samptoname_u: samptoname_u.o

schedtest: schedtest.o kutrace_lib.o
	$(CXX) $(CXXFLAGS) -pthread $< kutrace_lib.o -o $@

server4: server4.o dclab_log.o dclab_rpc.o kutrace_lib.o spinlock.o
	$(CXX) $(CXXFLAGS) -pthread $< dclab_log.o dclab_rpc.o kutrace_lib.o \
           spinlock.o -o $@

server_disk: server_disk.o dclab_log.o dclab_rpc.o kutrace_lib.o spinlock_fixed.o
	$(CXX) $(CXXFLAGS) -pthread $< dclab_log.o dclab_rpc.o kutrace_lib.o \
           spinlock_fixed.o -o $@

server_mystery21: server_mystery21.o dclab_log.o dclab_rpc.o kutrace_lib.o spinlock_fixed.o
	$(CXX) $(CXXFLAGS) -pthread $< \
           dclab_log.o dclab_rpc.o kutrace_lib.o spinlock_fixed.o \
           -o $@

spantospan: spantospan.o
spantoprof: spantoprof.o

spantotrim: spantotrim.o from_base40.o
	$(CXX) $(CXXFLAGS) $< from_base40.o -o $@


timealign: timealign.o dclab_log.o dclab_rpc.o kutrace_lib.o
	$(CXX) $(CXXFLAGS) $<  dclab_log.o dclab_rpc.o kutrace_lib.o -o $@

time_getpid: time_getpid.o kutrace_lib.o
	$(CXX) $(CXXFLAGS) $< kutrace_lib.o -o $@

unmakeself: unmakeself.o

whetstone_ku: whetstone_ku.o kutrace_lib.o
	$(CXX) $(CXXFLAGS) $< kutrace_lib.o -o $@

whetstone_ku.o: whetstone_ku.c
	$(CXX) -c $(CXXFLAGS) $< -o $@


$(OBJS): %.o: %.cc
	$(CXX) -c $(CXXFLAGS) $< -o $@


show:
	@echo "SRC:      $(SRC)"
	@echo "OBJS:     $(OBJS)"
	@echo "TARGETS:  $(TARGETS)"

clean:
	rm -f $(OBJS) $(TARGETS)

.PHONY: clean show

