#ifndef TEST_CASE_RIJNDAEL_H
#define TEST_CASE_RIJNDAEL_H
#include <stdint.h>
int rijndaelSetupEncrypt(uint32_t *, const uint8_t *, int);
int rijndaelSetupDecrypt(uint32_t *, const uint8_t *, int);
void rijndaelEncrypt(const uint32_t *, int, const uint8_t *, uint8_t *);
void rijndaelDecrypt(const uint32_t *, int, const uint8_t *, uint8_t *);
#endif
