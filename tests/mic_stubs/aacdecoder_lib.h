// SPDX-License-Identifier: GPL-3.0-only
#ifndef TEST_AACDECODER_LIB_H
#define TEST_AACDECODER_LIB_H
#include <stdint.h>
typedef unsigned char UCHAR;
typedef unsigned int UINT;
typedef int16_t INT_PCM;
typedef void *HANDLE_AACDECODER;
typedef int AAC_DECODER_ERROR;
typedef struct { int sampleRate, frameSize, numChannels, aot; } CStreamInfo;
#define TT_MP4_RAW 0
#define AAC_DEC_OK 0
#define AACDEC_CLRHIST 8u
#define AAC_TPDEC_CLEAR_BUFFER 0x0603
HANDLE_AACDECODER aacDecoder_Open(int transport, UINT layers);
void aacDecoder_Close(HANDLE_AACDECODER decoder);
AAC_DECODER_ERROR aacDecoder_ConfigRaw(HANDLE_AACDECODER decoder, UCHAR **config, UINT *size);
AAC_DECODER_ERROR aacDecoder_SetParam(HANDLE_AACDECODER decoder, int param, int value);
AAC_DECODER_ERROR aacDecoder_Fill(HANDLE_AACDECODER decoder, UCHAR **input, UINT *size, UINT *valid);
AAC_DECODER_ERROR aacDecoder_DecodeFrame(HANDLE_AACDECODER decoder, INT_PCM *pcm, int capacity, UINT flags);
CStreamInfo *aacDecoder_GetStreamInfo(HANDLE_AACDECODER decoder);
#endif
