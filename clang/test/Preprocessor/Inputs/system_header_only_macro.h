// Helper header to test system_header_only pragma
// This file should be included with -isystem to make it a system header.

#ifdef _TEST_MACRO
#define IN_SYSTEM_HEADER_VALUE 1
#else
#define IN_SYSTEM_HEADER_VALUE 0
#endif
