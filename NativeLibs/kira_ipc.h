#ifndef KIRA_IPC_H
#define KIRA_IPC_H

#include <stdint.h>

/* The three things a typed IPC format needs from the platform and cannot get
 * from Kira itself.
 *
 * Everything else in KiraIpc -- the tags, the varints, the field skipping, the
 * generated encoders -- is ordinary Kira, so it runs identically on the VM, on
 * LLVM, and under WASM. Only what genuinely has no Kira spelling is here:
 *
 *   1. A Float's bits. Kira has no reinterpret, and a wire format that wrote a
 *      Float as decimal text would lose the low bits of every value it carried.
 *   2. A String from bytes. Kira strings are byte-addressed for reading and have
 *      no byte-wise constructor, so a decoder holding UTF-8 has no way back.
 *   3. A framed duplex channel. Sockets are the platform's, not the language's.
 *
 * Nothing here knows what a message is. The channel moves opaque frames and the
 * two helpers convert one scalar each; the format lives entirely on the Kira
 * side, where it can be read. */

/* --- Bit-exact Float ------------------------------------------------------ */

/* A Float's IEEE-754 bits, and the way back. Kira's `Float` is a C `double` at
 * the seam, so this is a reinterpret and not a conversion: every value round
 * trips, including the infinities, the negative zero, and every NaN payload. */
uint64_t kira_ipc_f64_bits(double value);
double kira_ipc_f64_from_bits(uint64_t bits);

/* --- Text from bytes ------------------------------------------------------ */

/* Assemble `count` bytes into a NUL-terminated string a Kira `String` is built
 * from.
 *
 * The returned pointer addresses storage owned by this library and stays valid
 * until the next call on the same thread, which is exactly as long as the seam
 * needs: Kira copies the bytes into its own `String` as the call returns.
 *
 * A `String` on the wire is length-prefixed, so its bytes are recovered exactly.
 * What cannot survive this hop is an interior NUL: it terminates the C string
 * early and the tail is lost. That is why the format has a `Bytes` field type --
 * arbitrary binary belongs there, where it stays a byte array on both sides and
 * never becomes text. A `String` field carries text, and text has no U+0000. */
const char *kira_ipc_text_from_bytes(const uint8_t *bytes, int32_t count);

/* --- The channel ---------------------------------------------------------- */

/* A duplex, ordered, frame-preserving byte channel between two processes.
 *
 * Frames rather than a stream, because the layer above is typed: a reader that
 * had to discover where a message ended would be parsing the format twice, once
 * to find the boundary and once to mean it. A frame is a 4-byte little-endian
 * length and that many bytes, written and read whole.
 *
 * Every function answers on any platform. Where a platform has no
 * implementation the calls fail rather than trap, so a build links and runs and
 * an application sees a channel that will not connect -- which is the same thing
 * it sees when the peer is not installed, and the case it already handles. */

/* An id naming a live endpoint or channel. Zero is never one: it is what every
 * failing call answers, so a caller tests the id it was given rather than
 * remembering a different sentinel per function. */
typedef int32_t kira_ipc_id;

/* Publish an endpoint at `name` and start accepting. `name` is a rendezvous
 * name, not a path: each platform turns it into whatever it listens on. */
kira_ipc_id kira_ipc_listen(const char *name);

/* The next connection waiting on `endpoint`, or 0 when none is. Never blocks. */
kira_ipc_id kira_ipc_accept(kira_ipc_id endpoint);

/* Connect to the endpoint published at `name`, or 0 when nothing is there.
 *
 * Nothing there is the ordinary case rather than an error: it is what a shell
 * finds when the AI runtime is not installed, and it is answered the same way as
 * a runtime that is installed but has not come up yet. */
kira_ipc_id kira_ipc_connect(const char *name);

/* Send one frame. Answers 1 when the whole frame was handed to the platform and
 * 0 when it was not, which includes a peer that has gone. */
int32_t kira_ipc_send(kira_ipc_id channel, const uint8_t *bytes, int32_t count);

/* --- Handles ---------------------------------------------------------------
 *
 * Send one frame carrying one OS HANDLE beside its bytes.
 *
 * This exists because a GPU surface cannot be described by a number. A handle
 * naming shared memory, a dmabuf, or a texture is meaningful only inside the
 * process that owns it: putting its value in a message and reading it in the
 * peer names whatever that number happens to be over there, which is a valid
 * handle to something else. Every platform therefore has a way to TRANSFER one,
 * and every one of them is a property of the channel rather than of the bytes:
 *
 *   POSIX    the handle rides the socket as an SCM_RIGHTS control message, and
 *            the kernel installs a new descriptor in the receiver.
 *   Windows  the sender duplicates it into the peer's process, and the
 *            duplicated VALUE travels in the frame -- which is legitimate there
 *            precisely because the duplication already made it the peer's.
 *
 * So the transfer is here, at the channel, and a message carries only an id
 * saying which transferred handle it means.
 *
 * Handles arrive IN ORDER with the frames that carried them, so a frame's
 * handle is claimed with `kira_ipc_frame_handle` while that frame is the one
 * `kira_ipc_poll` is answering about. A frame released without its handle being
 * claimed has that handle CLOSED rather than leaked -- a receiver that does not
 * understand a message must not slowly exhaust its descriptor table. */
int32_t kira_ipc_send_handle(kira_ipc_id channel, const uint8_t *bytes, int32_t count, int64_t handle);

/* The handle that arrived with the frame now waiting, or 0 when it carried
 * none. Answering it hands ownership to the caller, which is then what closes
 * it. Asking twice for one frame's handle answers 0 the second time. */
int64_t kira_ipc_frame_handle(kira_ipc_id channel);

/* The byte length of the frame now waiting on `channel`, or 0 when none is.
 * Never blocks, and never returns a partial frame: a frame that has only partly
 * arrived is still 0, and the bytes stay buffered until the rest lands. */
int32_t kira_ipc_poll(kira_ipc_id channel);

/* Eight bytes of the waiting frame, little-endian, starting at `index * 8`.
 *
 * A word rather than a byte because this is the one call a decoder makes per
 * eight bytes of message, and the frames it makes it on are control messages of
 * a few hundred bytes. Bulk pixels never come this way: they go through a shared
 * surface, and what crosses here is the handle naming it.
 *
 * Reading past the frame answers zero rather than trapping, so the tail word of
 * a frame whose length is not a multiple of eight reads as the bytes it has. */
uint64_t kira_ipc_frame_word(kira_ipc_id channel, int32_t index);

/* Drop the waiting frame and let the next one arrive. */
void kira_ipc_frame_release(kira_ipc_id channel);

/* Whether the peer is still there. A channel whose peer has gone answers 0 and
 * keeps answering it; it is closed by its owner, never by this. */
int32_t kira_ipc_alive(kira_ipc_id channel);

/* Close a channel or an endpoint. Closing something already closed does
 * nothing, so shutdown does not have to be ordered against a peer's. */
void kira_ipc_close(kira_ipc_id channel);

#endif
