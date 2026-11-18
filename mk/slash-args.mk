$(BUILD)/dd_slash_args_tests: $(CORE) src/tools/slash_args.cpp include/dd/tools/slash_args.hpp tests/slash_args_test.cpp
	$(CXX) $(CXXFLAGS) $(CORE) src/tools/slash_args.cpp tests/slash_args_test.cpp -pthread -o $(BUILD)/dd_slash_args_tests

TEST_BINS += $(BUILD)/dd_slash_args_tests
