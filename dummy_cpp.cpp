#include "dummy_cpp.h"

extern "C" {

int dummy_cpp_function(int value) {
    return value * 2;
}

}
