#include <cstdlib>

int main() {
  int* p = static_cast<int*>(std::malloc(sizeof(int)));
  *p = 42;
  std::free(p);
  return *p;
}
