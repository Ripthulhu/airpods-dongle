/* Third-Party Modified Version: AirPods Dongle, 2026-10-30.
 * The AAC-only profile has no SBR/MPS lookahead or input offset. */
#ifndef FDK_DONGLE_INPUT_BUFFER_H
#define FDK_DONGLE_INPUT_BUFFER_H

static inline AACENC_ERROR fdk_dongle_input_buffer(INT_PCM **buffer, INT *stride,
                                                  INT *total, INT channels,
                                                  INT frameLength) {
  if (channels < 1 || channels > 2 || frameLength < 1 || frameLength > 1024 ||
      (*buffer == NULL && (*stride != 0 || *total != 0)) ||
      (*buffer != NULL && (*stride < 1 || *stride > 1024 ||
                           *total != channels * *stride))) {
    return AACENC_INVALID_CONFIG;
  }
  if (*buffer != NULL && *stride >= frameLength) return AACENC_OK;

  INT_PCM *next = (INT_PCM *)FDKcalloc(channels * frameLength, sizeof(INT_PCM));
  if (next == NULL) return AACENC_MEMORY_ERROR;
  INT_PCM *previous = *buffer;
  if (previous != NULL) {
    for (INT channel = 0; channel < channels; ++channel) {
      FDKmemcpy(next + channel * frameLength, previous + channel * *stride,
                *stride * sizeof(INT_PCM));
    }
  }
  *buffer = next;
  *stride = frameLength;
  *total = channels * frameLength;
  if (previous != NULL) FDKfree(previous);
  return AACENC_OK;
}

#endif
