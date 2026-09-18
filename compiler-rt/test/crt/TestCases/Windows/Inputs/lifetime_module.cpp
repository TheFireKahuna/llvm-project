#include <stdio.h>

struct Object {
  int id;
  explicit Object(int value) : id(value) {
    fprintf(stderr, "construct %d\n", id);
  }
  ~Object() { fprintf(stderr, "destroy %d\n", id); }
};

extern "C" __declspec(dllexport) void construct(int id) {
  static Object object(id);
}
