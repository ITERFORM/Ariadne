$(BUILD)/dd_clip_tests: include/dd/clip.hpp src/clip.cpp $(ANSI) tests/clip_test.cpp
	$(CXX) $(CXXFLAGS) src/clip.cpp $(ANSI) tests/clip_test.cpp -pthread -o $(BUILD)/dd_clip_tests

TEST_BINS += $(BUILD)/dd_clip_tests
