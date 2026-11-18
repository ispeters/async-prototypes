#include "iface.hpp"
bool remote::get_entry(int key, cont k, int* slot) noexcept {
  if (key & 1) { *slot = key * 2; return true; }
  k.resume(k.frame, key * 2); return false;
}
int remote::get_sync(int key) noexcept { return key * 2; }
