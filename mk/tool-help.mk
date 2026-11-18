$(BUILD)/dd_tool_help_tests: $(CORE) src/tools/tool_help.cpp include/dd/tools/tool_help.hpp tests/tool_help_test.cpp
	$(CXX) $(CXXFLAGS) $(CORE) src/tools/tool_help.cpp tests/tool_help_test.cpp -pthread -o $(BUILD)/dd_tool_help_tests

TEST_BINS += $(BUILD)/dd_tool_help_tests
