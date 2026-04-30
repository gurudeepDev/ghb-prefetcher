CXX      = g++
CXXFLAGS = -O2 -std=c++14 -Wall
TARGET   = sim
SRC      = simulator.cpp

all: $(TARGET)

$(TARGET): $(SRC)
	$(CXX) $(CXXFLAGS) -o $(TARGET) $(SRC)
	@echo ""
	@echo "Build complete! Run:"
	@echo "  ./sim              — full simulation (all prefetchers × benchmarks)"
	@echo "  ./sim --demo       — step-by-step algorithm walkthrough"

clean:
	rm -f $(TARGET)

.PHONY: all clean
