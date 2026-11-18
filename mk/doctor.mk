# mk/doctor.mk — `ariadne doctor`: stack.conf, model lookup, launch lines, server processes and
# mazemaker's AFE wake protocol. No GPU and no model: a `sleep` stands in for a server.
DOCTOR_TEST_SRCS := src/doctor/stack_conf.cpp src/doctor/stack.cpp src/doctor/afe.cpp src/doctor/bench.cpp \
                    src/limbo/spawn.cpp src/core/json.cpp $(NET)

$(BUILD)/dd_doctor_tests: $(DOCTOR_TEST_SRCS) include/dd/doctor/stack_conf.hpp include/dd/doctor/stack.hpp \
                          include/dd/doctor/afe.hpp include/dd/doctor/bench.hpp include/dd/limbo/spawn.hpp tests/doctor_test.cpp
	$(CXX) $(CXXFLAGS) $(sort $(DOCTOR_TEST_SRCS)) tests/doctor_test.cpp -pthread $(CURL_LIBS) $(TLS_LIBS) \
		-o $(BUILD)/dd_doctor_tests

TEST_BINS += $(BUILD)/dd_doctor_tests
