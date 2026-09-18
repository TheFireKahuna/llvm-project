#ifndef ITANIUM_DLL_CONTRACT_H
#define ITANIUM_DLL_CONTRACT_H

#include <stdexcept>

#ifdef BUILD_DLL
#  define CONTRACT_API __declspec(dllexport)
#else
#  define CONTRACT_API __declspec(dllimport)
#endif

struct CONTRACT_API Root {
  virtual ~Root();
  int root = 10;
};
struct CONTRACT_API Left : virtual Root {
  virtual int left() const;
};
struct CONTRACT_API Right : virtual Root {
  virtual int right() const;
};
struct CONTRACT_API Derived final : Left, Right {
  ~Derived() override;
  int value = 42;
};
struct CONTRACT_API Error : std::runtime_error {
  explicit Error(int);
  ~Error() override;
  int value;
};
struct alignas(256) Block {
  unsigned char bytes[256];
};

extern "C" {
CONTRACT_API Root *make_object();
CONTRACT_API void throw_error();
CONTRACT_API int catch_callback(void (*)());
CONTRACT_API int destruction_count();
CONTRACT_API Block *make_block();
CONTRACT_API void delete_block(Block *);
CONTRACT_API bool check_new_handler(void (*)());
CONTRACT_API void fail_allocation();
}
#endif
