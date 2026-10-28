/// @file
/// Small debug-bearing image inspected by native delivery tests; never executed by the test.

__attribute__((noinline)) int DeliveryFixtureLine(int value) {
  return value + 7;
}

int main() {
  return DeliveryFixtureLine(1);
}
