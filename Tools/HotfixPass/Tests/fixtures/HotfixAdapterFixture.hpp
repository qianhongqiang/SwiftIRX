#ifndef HotfixAdapterFixture_hpp
#define HotfixAdapterFixture_hpp

#include <cstdint>

extern "C" std::int64_t ir_test_c_increment(std::int64_t value);

class IRTestCounter {
public:
  std::int64_t multiply(std::int64_t value);
};

#endif
