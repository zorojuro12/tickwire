#include <thread>

int g_counter = 0;

void increment() {
  for (int i = 0; i < 100000; ++i) {
    ++g_counter;
  }
}

int main() {
  std::thread t1(increment);
  std::thread t2(increment);
  t1.join();
  t2.join();
  return 0;
}
