# cleantests 	- clean local and subdirectories' tests
# buildtests	- build all local and subdirectories' tests
# runtests	- run all local tests 
# testall	- clean, build and run local and subdirectories' tests

include ../../build_gateway.inc
include ../../makefile.inc
include ../../test.inc

CC = gcc
CPP = g++

TESTPATH 	:= $(shell pwd)
TESTLOG		:= $(TESTPATH)/testlog.log	
LOG_MANAGER_PATH:= $(ROOT_PATH)/log_manager
TESTAPP 	:= $(TESTPATH)/testlog
UTILS_PATH	:= $(ROOT_PATH)/utils

# Use two threads by default
ifndef NTHR
	NTHR=2
endif

testall: 
	$(MAKE) cleantests
	$(MAKE) DEBUG=Y buildtests 
	$(MAKE) runtests

cleantests:
	- $(DEL) *.o 
	- $(DEL) *.log 
	- $(DEL) testlog
	- $(DEL) testorder	
	- $(DEL) *~

buildtests:
	$(MAKE) -C $(LOG_MANAGER_PATH) ADD_DEBUG_TAGS=Y
	$(CC) $(CFLAGS)	 \
	-L$(LOG_MANAGER_PATH) \
	-Wl,-rpath,$(DEST)/lib \
	-Wl,-rpath,$(LOG_MANAGER_PATH)/ \
	-o testlog \
	-I$(MARIADB_SRC_PATH)/include \
	-I$(LOG_MANAGER_PATH) -I$(UTILS_PATH) testlog.c \
	-lstdc++ -llog_manager $(LDLIBS) \
	$(UTILS_PATH)/skygw_utils.o
	$(CC) $(CFLAGS)	 \
	-L$(LOG_MANAGER_PATH) \
	-Wl,-rpath,$(DEST)/lib \
	-Wl,-rpath,$(LOG_MANAGER_PATH)/ \
	-o testorder \
	-I$(MARIADB_SRC_PATH)/include \
	-I$(LOG_MANAGER_PATH) -I$(UTILS_PATH) testorder.c \
	-lstdc++ -llog_manager $(LDLIBS) \
	$(UTILS_PATH)/skygw_utils.o




runtests: 
	@echo ""				>  $(TESTLOG)
	@echo "-------------------------------"	>> $(TESTLOG)
	@echo $(shell date)			>> $(TESTLOG)
	@echo "Test Log Manager"		>> $(TESTLOG)
	@echo "-------------------------------"	>> $(TESTLOG)
	@echo ""				>> $(TESTLOG)
	@echo "Use 1 thread"			>> $(TESTLOG)
	@echo ""				>> $(TESTLOG)
	@-$(LAUNCH_DEBUGGER) $(TESTAPP) "-t 1"	2>>$(TESTLOG)
	@echo ""				>> $(TESTLOG)
	@echo "Use 8 threads"			>> $(TESTLOG)
	@echo ""				>> $(TESTLOG)
	@-$(LAUNCH_DEBUGGER) $(TESTAPP) "-t 8"	2>>$(TESTLOG)
	@echo ""				>> $(TESTLOG)
	@echo "Use 16 threads"			>> $(TESTLOG)
	@echo ""				>> $(TESTLOG)
	@-$(LAUNCH_DEBUGGER) $(TESTAPP) "-t 16"	2>>$(TESTLOG)
	@echo ""				>>  $(TEST_MAXSCALE_LOG)
	@echo "Test Message Order"  >> $(TEST_MAXSCALE_LOG)
	@echo ""				>>  $(TEST_MAXSCALE_LOG)
	./logorder.sh 500 0 500 $(TEST_MAXSCALE_LOG)
	@echo "Log Manager PASSED"		>> $(TESTLOG)
	@echo ""				>> $(TESTLOG)

#	@cat $(TESTLOG)				>> $(TEST_MAXSCALE_LOG)
