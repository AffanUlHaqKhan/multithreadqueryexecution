CXX      ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -O2
LDLIBS   = -lcurl -lcrypto -pthread

PROGRAMS = neo4j_http_example \
           neo4j_threaded_query_example \
           neo4j_threaded_query_example_1 \
           neo4j_threaded_query_union_delimitter

TEST_BIN = tests/test_union_delimitter

.PHONY: all test clean

all: $(PROGRAMS)

%: %.cpp
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDLIBS)

$(TEST_BIN): tests/test_union_delimitter.cpp neo4j_threaded_query_union_delimitter.cpp
	$(CXX) $(CXXFLAGS) -DUNIT_TEST $< -o $@ -lgtest -lgtest_main $(LDLIBS)

test: $(TEST_BIN)
	./$(TEST_BIN)

clean:
	rm -f $(PROGRAMS) $(TEST_BIN)
