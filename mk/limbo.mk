# mk/limbo.mk — `ariadne limbo`: stack.conf, model lookup, launch lines, server processes and
# mazemaker's AFE wake protocol. No GPU and no model: a `sleep` stands in for a server.
LIMBO_TEST_SRCS := src/limbo/stack_conf.cpp src/limbo/stack.cpp src/limbo/afe.cpp src/limbo/bench.cpp \
                   src/limbo/spawn.cpp src/core/json.cpp $(NET)

$(BUILD)/dd_limbo_tests: $(LIMBO_TEST_SRCS) include/dd/limbo/stack_conf.hpp include/dd/limbo/stack.hpp \
                         include/dd/limbo/afe.hpp include/dd/limbo/bench.hpp include/dd/limbo/spawn.hpp tests/limbo_test.cpp
	$(CXX) $(CXXFLAGS) $(sort $(LIMBO_TEST_SRCS)) tests/limbo_test.cpp -pthread $(CURL_LIBS) $(TLS_LIBS) \
		-o $(BUILD)/dd_limbo_tests

TEST_BINS += $(BUILD)/dd_limbo_tests
