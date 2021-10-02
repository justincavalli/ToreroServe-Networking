CXX=g++
CXXFLAGS=-Wall -Wextra -g -O1 -std=c++17 -pthread

TARGETS=torero-serve
PC_SRC= BoundedBuffer.cpp torero-serve.cpp

all: $(TARGETS)

torero-serve: $(PC_SRC) BoundedBuffer.hpp
	$(CXX) $^ -o $@ $(CXXFLAGS)
clean:
	rm -f $(TARGETS)
