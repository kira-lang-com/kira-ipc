/* The two scalar conversions Kira has no spelling for. Both are pure and
 * platform-independent; see kira_ipc.h for why they are here at all. */

#include "kira_ipc.h"

#include <stdlib.h>
#include <string.h>

uint64_t kira_ipc_f64_bits(double value) {
    uint64_t bits = 0;
    /* memcpy rather than a union or a cast through a pointer: it is the one
     * spelling every compiler lowers to a register move without invoking
     * strict-aliasing undefined behaviour. */
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

double kira_ipc_f64_from_bits(uint64_t bits) {
    double value = 0.0;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

/* The buffer the assembled text is handed back in.
 *
 * One per thread, because two threads decoding at once would otherwise write
 * over each other's answer, and a decoder is exactly the thing an application
 * runs on whatever thread its channel is pumped on.
 *
 * It grows to the largest string that has crossed and is never shrunk. A wire
 * string is length-prefixed and bounded by its frame, so the high-water mark is
 * a frame, not a leak. */
#if defined(_MSC_VER)
#define KIRA_IPC_THREAD_LOCAL __declspec(thread)
#else
#define KIRA_IPC_THREAD_LOCAL __thread
#endif

/* Held rather than reallocated per call: assembling a string is on the decode
 * path of every message that carries one. */
static KIRA_IPC_THREAD_LOCAL char *kira_ipc_text = 0;
static KIRA_IPC_THREAD_LOCAL int32_t kira_ipc_text_capacity = 0;

const char *kira_ipc_text_from_bytes(const uint8_t *bytes, int32_t count) {
    if (count < 0) {
        count = 0;
    }
    if (count + 1 > kira_ipc_text_capacity) {
        char *grown = (char *)realloc(kira_ipc_text, (size_t)(count + 1));
        if (grown == 0) {
            /* Out of memory answering a decode. An empty string is a value the
             * caller can carry; a null pointer is one it would dereference. */
            return "";
        }
        kira_ipc_text = grown;
        kira_ipc_text_capacity = count + 1;
    }
    if (count > 0 && bytes != 0) {
        memcpy(kira_ipc_text, bytes, (size_t)count);
    }
    kira_ipc_text[count] = 0;
    return kira_ipc_text;
}
