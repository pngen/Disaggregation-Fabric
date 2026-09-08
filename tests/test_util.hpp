#pragma once
#include <cstdio>
#include <cstdlib>
#define CHECK(cond) do { if (!(cond)) { std::fflush(stdout); std::fprintf(stderr, "CHECK FAILED: %s (%s:%d)\n", #cond, __FILE__, __LINE__); std::fflush(stderr); std::abort(); } } while(0)
