/* The framed duplex channel.
 *
 * One table of slots, indexed by the id the Kira side holds. An id is the slot
 * index plus one, so zero is never a live channel and every failing call can
 * answer it without a second sentinel.
 *
 * A frame is a 4-byte little-endian length, eight bytes of handle, and then the
 * body. The top bit of the length says whether the handle is real; the eight
 * bytes are present either way, so the body sits at a known offset and nothing
 * has to parse the header twice to find it. The
 * reader accumulates whatever the platform gives it and only ever publishes a
 * frame once all of it has arrived, so the layer above never sees a partial
 * message and never has to ask whether it has one.
 *
 * Nothing here blocks. A shell pumps its channels once per frame beside its
 * window, and a blocking read would be a frame the window did not draw. */

#include "kira_ipc.h"

#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

/* How many endpoints and channels one process may hold at once.
 *
 * A generous ceiling on a number that is small by construction: a shell holds
 * one channel per component it has, and a component holds one per shell. The
 * table is fixed rather than grown because a growing table would move the slots
 * an id names. */
#define KIRA_IPC_SLOTS 64

/* The largest frame that will be accepted, in bytes.
 *
 * A control message is a few hundred bytes; a megabyte is far past anything the
 * format is for and well short of anything that threatens the process. The cap
 * exists because a length prefix arrives from another process: a peer that is
 * confused, or hostile, otherwise names an allocation this side must make. */
#define KIRA_IPC_MAX_FRAME (1024 * 1024)

/* How many transferred handles may be in flight on one channel at once.
 *
 * A surface is handed over when it is created or resized, which is rare next to
 * the messages about it. A queue this deep is already far past what a peer that
 * is keeping up would ever hold; one that is not keeping up is refused a
 * further handle rather than being allowed to grow this side's descriptor
 * table without bound. */
#define KIRA_IPC_HANDLE_QUEUE 16

/* The top bit of the length word says the frame carries a handle. Free by
 * construction: a frame is capped at a megabyte, so bit 31 is never a length. */
#define KIRA_IPC_HANDLE_FLAG 0x80000000u

/* Every frame reserves eight bytes for a handle value between its length and its
 * body, whether or not it carries one. Present either way so the body's offset
 * is one constant rather than a thing to work out per frame. */
#define KIRA_IPC_HEADER 12

enum kira_ipc_kind {
    KIRA_IPC_FREE = 0,
    KIRA_IPC_ENDPOINT = 1,
    KIRA_IPC_CHANNEL = 2
};

struct kira_ipc_slot {
    int kind;
    int alive;
#if defined(_WIN32)
    HANDLE handle;
    /* One read is kept in flight at all times. `pending` says whether it is,
     * `overlapped` and `event` are its state, and `chunk` is where it lands. */
    OVERLAPPED overlapped;
    HANDLE event;
    int pending;
    uint8_t chunk[4096];
    /* An endpoint keeps the name so it can arm the next instance after handing
     * the current one to an accepted channel. */
    char name[256];
    int connecting;
#else
    int fd;
    char name[256];
#endif
    /* What has arrived and not yet been consumed, and how much of it there is.
     * The frame at the front is published once `filled` covers its length. */
    uint8_t *buffer;
    int32_t capacity;
    int32_t filled;
    /* Handles that arrived and have not been claimed yet, oldest first.
     *
     * A queue rather than a single slot because handles arrive IN ORDER with
     * the frames that carried them, and a receiver is allowed to be a frame or
     * two behind: the platform delivers a descriptor as soon as the bytes it
     * rode with land, which may be before the frame ahead of it is consumed. */
    int64_t handles[KIRA_IPC_HANDLE_QUEUE];
    int32_t handleCount;
    /* Whether the front frame's handle has already been handed over. */
    int32_t frontHandleTaken;
};

static struct kira_ipc_slot kira_ipc_slots[KIRA_IPC_SLOTS];

static struct kira_ipc_slot *kira_ipc_slot_at(kira_ipc_id id, int kind) {
    if (id <= 0 || id > KIRA_IPC_SLOTS) {
        return 0;
    }
    struct kira_ipc_slot *slot = &kira_ipc_slots[id - 1];
    if (slot->kind != kind) {
        return 0;
    }
    return slot;
}

static kira_ipc_id kira_ipc_slot_claim(int kind) {
    for (int index = 0; index < KIRA_IPC_SLOTS; index += 1) {
        if (kira_ipc_slots[index].kind == KIRA_IPC_FREE) {
            struct kira_ipc_slot *slot = &kira_ipc_slots[index];
            memset(slot, 0, sizeof(*slot));
            slot->kind = kind;
            slot->alive = 1;
            return (kira_ipc_id)(index + 1);
        }
    }
    return 0;
}

/* An endpoint this process already published under `name`, or 0.
 *
 * Publishing one name twice from one process is always a mistake, and it is a
 * mistake with a delayed and unrecognisable symptom: the second bind unlinks the
 * first one's socket file, so the endpoint that peers are connecting to stops
 * existing while the process still believes it is listening. Answering the
 * endpoint that is already open makes the call idempotent, which is what a
 * caller that cannot easily tell whether it has run before actually needs. */
static kira_ipc_id kira_ipc_published(const char *name) {
    if (name == 0) {
        return 0;
    }
    for (int index = 0; index < KIRA_IPC_SLOTS; index += 1) {
        struct kira_ipc_slot *slot = &kira_ipc_slots[index];
        if (slot->kind == KIRA_IPC_ENDPOINT && strcmp(slot->name, name) == 0) {
            return (kira_ipc_id)(index + 1);
        }
    }
    return 0;
}

/* Make room for `needed` more bytes at the tail of the receive buffer. */
static int kira_ipc_reserve(struct kira_ipc_slot *slot, int32_t needed) {
    if (slot->filled + needed <= slot->capacity) {
        return 1;
    }
    int32_t capacity = slot->capacity > 0 ? slot->capacity : 4096;
    while (capacity < slot->filled + needed) {
        capacity = capacity * 2;
    }
    uint8_t *grown = (uint8_t *)realloc(slot->buffer, (size_t)capacity);
    if (grown == 0) {
        return 0;
    }
    slot->buffer = grown;
    slot->capacity = capacity;
    return 1;
}

/* The length of the frame at the front of the buffer, or 0 when the buffer does
 * not yet hold a whole one. */
static uint32_t kira_ipc_front_word(struct kira_ipc_slot *slot) {
    return (uint32_t)slot->buffer[0]
        | ((uint32_t)slot->buffer[1] << 8)
        | ((uint32_t)slot->buffer[2] << 16)
        | ((uint32_t)slot->buffer[3] << 24);
}

/* Whether the frame at the front declared a handle. Only meaningful once
 * `kira_ipc_front_frame` has said there is a whole frame there. */
static int kira_ipc_front_carries_handle(struct kira_ipc_slot *slot) {
    if (slot->filled < KIRA_IPC_HEADER) {
        return 0;
    }
    return (kira_ipc_front_word(slot) & KIRA_IPC_HANDLE_FLAG) != 0;
}

/* The handle VALUE written into the front frame's header. Windows puts the
 * duplicated handle here; POSIX writes zero and sends the real descriptor as
 * ancillary data. */
static int64_t kira_ipc_front_header_handle(struct kira_ipc_slot *slot) {
    int64_t value = 0;
    for (int32_t step = 0; step < 8; step += 1) {
        value |= (int64_t)slot->buffer[4 + step] << (step * 8);
    }
    return value;
}

static int32_t kira_ipc_front_frame(struct kira_ipc_slot *slot) {
    if (slot->filled < KIRA_IPC_HEADER) {
        return 0;
    }
    uint32_t length = kira_ipc_front_word(slot) & ~KIRA_IPC_HANDLE_FLAG;
    if (length > (uint32_t)KIRA_IPC_MAX_FRAME) {
        /* A length this side will not honour. The peer and this process no
         * longer agree on where messages begin, and there is no resynchronising
         * a stream whose framing is gone: the channel is done. */
        slot->alive = 0;
        return 0;
    }
    if (slot->filled < KIRA_IPC_HEADER + (int32_t)length) {
        return 0;
    }
    return (int32_t)length;
}

/* Lay out a frame's header: its length, whether a handle rides with it, and the
 * handle value a platform that passes one by value writes there. */
static void kira_ipc_write_header(uint8_t *header, int32_t count, int carries, int64_t handle) {
    uint32_t word = (uint32_t)count;
    if (carries) {
        word |= KIRA_IPC_HANDLE_FLAG;
    }
    header[0] = (uint8_t)(word & 0xFF);
    header[1] = (uint8_t)((word >> 8) & 0xFF);
    header[2] = (uint8_t)((word >> 16) & 0xFF);
    header[3] = (uint8_t)((word >> 24) & 0xFF);
    for (int32_t step = 0; step < 8; step += 1) {
        header[4 + step] = (uint8_t)((handle >> (step * 8)) & 0xFF);
    }
}

/* Take the oldest unclaimed handle off the queue. */
static int64_t kira_ipc_handle_pop(struct kira_ipc_slot *slot) {
    if (slot->handleCount <= 0) {
        return 0;
    }
    int64_t handle = slot->handles[0];
    for (int32_t index = 1; index < slot->handleCount; index += 1) {
        slot->handles[index - 1] = slot->handles[index];
    }
    slot->handleCount -= 1;
    return handle;
}

#if !defined(_WIN32)

/* The path an endpoint name is published at.
 *
 * The runtime directory when the session has one, because that is the directory
 * the platform already clears when the session ends. `/tmp` otherwise, which is
 * where a socket goes on a system that names no better place. */
static int kira_ipc_socket_path(const char *name, char *out, size_t size) {
    const char *directory = getenv("XDG_RUNTIME_DIR");
    if (directory == 0 || directory[0] == 0) {
        /* macOS and the BSDs set no runtime directory, and a Linux session
         * without one is ordinary too. `/tmp` is where a socket goes on a system
         * that names no better place. */
        directory = "/tmp";
    }
    int written = snprintf(out, size, "%s/kira-ipc-%s", directory, name != 0 ? name : "");
    /* `sun_path` is 104 bytes on the BSDs and macOS and 108 on Linux, and
     * truncation there is not an error the kernel reports: it binds the shorter
     * path, and two names that share a prefix become one endpoint. Refusing is
     * the only safe answer. */
    if (written < 0 || (size_t)written >= size) {
        return 0;
    }
    return 1;
}

static void kira_ipc_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
}

kira_ipc_id kira_ipc_listen(const char *name) {
    kira_ipc_id existing = kira_ipc_published(name);
    if (existing != 0) {
        return existing;
    }
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return 0;
    }
    struct sockaddr_un address;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    if (kira_ipc_socket_path(name, address.sun_path, sizeof(address.sun_path)) == 0) {
        close(fd);
        return 0;
    }
    /* A socket file left by a process that did not get to close it would stop
     * this one binding. Removing it is safe because binding is what proves the
     * name free: a live listener still holding it takes the name back below by
     * failing to bind. */
    unlink(address.sun_path);
    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
        close(fd);
        return 0;
    }
    if (listen(fd, 8) != 0) {
        close(fd);
        unlink(address.sun_path);
        return 0;
    }
    kira_ipc_nonblocking(fd);
    kira_ipc_id id = kira_ipc_slot_claim(KIRA_IPC_ENDPOINT);
    if (id == 0) {
        close(fd);
        unlink(address.sun_path);
        return 0;
    }
    struct kira_ipc_slot *published = kira_ipc_slot_at(id, KIRA_IPC_ENDPOINT);
    published->fd = fd;
    snprintf(published->name, sizeof(published->name), "%s", name != 0 ? name : "");
    return id;
}

kira_ipc_id kira_ipc_accept(kira_ipc_id endpoint) {
    struct kira_ipc_slot *slot = kira_ipc_slot_at(endpoint, KIRA_IPC_ENDPOINT);
    if (slot == 0) {
        return 0;
    }
    int fd = accept(slot->fd, 0, 0);
    if (fd < 0) {
        return 0;
    }
    kira_ipc_nonblocking(fd);
    kira_ipc_id id = kira_ipc_slot_claim(KIRA_IPC_CHANNEL);
    if (id == 0) {
        close(fd);
        return 0;
    }
    kira_ipc_slot_at(id, KIRA_IPC_CHANNEL)->fd = fd;
    return id;
}

kira_ipc_id kira_ipc_connect(const char *name) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return 0;
    }
    struct sockaddr_un address;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    if (kira_ipc_socket_path(name, address.sun_path, sizeof(address.sun_path)) == 0) {
        close(fd);
        return 0;
    }
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
        close(fd);
        return 0;
    }
    kira_ipc_nonblocking(fd);
    kira_ipc_id id = kira_ipc_slot_claim(KIRA_IPC_CHANNEL);
    if (id == 0) {
        close(fd);
        return 0;
    }
    kira_ipc_slot_at(id, KIRA_IPC_CHANNEL)->fd = fd;
    return id;
}

/* Write every byte, tolerating the short writes a non-blocking socket answers
 * with. A frame is written whole or the channel is finished: half a frame in the
 * peer's buffer is a stream whose framing is gone. */
static int kira_ipc_write_all(int fd, const uint8_t *bytes, int32_t count) {
    int32_t written = 0;
    while (written < count) {
        ssize_t step = write(fd, bytes + written, (size_t)(count - written));
        if (step > 0) {
            written += (int32_t)step;
            continue;
        }
        if (step < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            continue;
        }
        return 0;
    }
    return 1;
}

/* Send a frame, optionally handing a descriptor over with it.
 *
 * The header goes out through `sendmsg` so the descriptor can ride with it as an
 * SCM_RIGHTS control message. The kernel installs a NEW descriptor in the
 * receiver naming the same open file, which is what makes a dmabuf or a shared
 * memory object meaningful on the other side -- the number itself never would
 * be. The body follows as ordinary writes. */
static int32_t kira_ipc_send_frame(kira_ipc_id channel, const uint8_t *bytes, int32_t count, int carries, int64_t handle) {
    struct kira_ipc_slot *slot = kira_ipc_slot_at(channel, KIRA_IPC_CHANNEL);
    if (slot == 0 || slot->alive == 0 || count < 0 || count > KIRA_IPC_MAX_FRAME) {
        return 0;
    }
    uint8_t header[KIRA_IPC_HEADER];
    /* The value is not written into the header on this platform: the descriptor
     * itself travels beside the bytes, and a number naming it here would be this
     * process's number, which means nothing over there. */
    kira_ipc_write_header(header, count, carries, 0);

    struct iovec io;
    io.iov_base = header;
    io.iov_len = sizeof(header);
    struct msghdr message;
    memset(&message, 0, sizeof(message));
    message.msg_iov = &io;
    message.msg_iovlen = 1;
    union {
        struct cmsghdr align;
        char bytes[CMSG_SPACE(sizeof(int))];
    } control;
    if (carries) {
        memset(&control, 0, sizeof(control));
        message.msg_control = control.bytes;
        message.msg_controllen = sizeof(control.bytes);
        struct cmsghdr *header_message = CMSG_FIRSTHDR(&message);
        header_message->cmsg_level = SOL_SOCKET;
        header_message->cmsg_type = SCM_RIGHTS;
        header_message->cmsg_len = CMSG_LEN(sizeof(int));
        int descriptor = (int)handle;
        memcpy(CMSG_DATA(header_message), &descriptor, sizeof(descriptor));
    }
    ssize_t sent = 0;
    while (1) {
        sent = sendmsg(slot->fd, &message, 0);
        if (sent >= 0) {
            break;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            continue;
        }
        slot->alive = 0;
        return 0;
    }
    /* A short header write would split the control message from the frame it
     * belongs to, and there is no way to send the remainder with it. A header is
     * twelve bytes and a socket buffer is never that full, so this is a broken
     * channel rather than a case to handle. */
    if (sent != (ssize_t)sizeof(header)) {
        slot->alive = 0;
        return 0;
    }
    if (count > 0 && kira_ipc_write_all(slot->fd, bytes, count) == 0) {
        slot->alive = 0;
        return 0;
    }
    return 1;
}

int32_t kira_ipc_send(kira_ipc_id channel, const uint8_t *bytes, int32_t count) {
    return kira_ipc_send_frame(channel, bytes, count, 0, 0);
}

int32_t kira_ipc_send_handle(kira_ipc_id channel, const uint8_t *bytes, int32_t count, int64_t handle) {
    return kira_ipc_send_frame(channel, bytes, count, 1, handle);
}

/* Drain whatever the socket is holding into the receive buffer. */
static void kira_ipc_fill(struct kira_ipc_slot *slot) {
    while (1) {
        if (kira_ipc_reserve(slot, 4096) == 0) {
            return;
        }
        struct iovec io;
        io.iov_base = slot->buffer + slot->filled;
        io.iov_len = 4096;
        struct msghdr message;
        memset(&message, 0, sizeof(message));
        message.msg_iov = &io;
        message.msg_iovlen = 1;
        union {
            struct cmsghdr align;
            char bytes[CMSG_SPACE(sizeof(int)) * 4];
        } control;
        memset(&control, 0, sizeof(control));
        message.msg_control = control.bytes;
        message.msg_controllen = sizeof(control.bytes);
        ssize_t step = recvmsg(slot->fd, &message, 0);
        if (step > 0) {
            /* Collect every descriptor that rode with these bytes. They are
             * queued rather than matched to a frame here: they arrive in the
             * order their frames did, and the frame they belong to may not have
             * been consumed yet. */
            struct cmsghdr *carried = CMSG_FIRSTHDR(&message);
            while (carried != 0) {
                if (carried->cmsg_level == SOL_SOCKET && carried->cmsg_type == SCM_RIGHTS) {
                    int32_t room = (int32_t)((carried->cmsg_len - CMSG_LEN(0)) / sizeof(int));
                    for (int32_t index = 0; index < room; index += 1) {
                        int descriptor = 0;
                        memcpy(&descriptor, CMSG_DATA(carried) + index * sizeof(int), sizeof(descriptor));
                        if (slot->handleCount < KIRA_IPC_HANDLE_QUEUE) {
                            slot->handles[slot->handleCount] = (int64_t)descriptor;
                            slot->handleCount += 1;
                        } else {
                            /* A peer handing over surfaces faster than this side
                             * claims them. Closing is the only answer that does
                             * not grow this process's descriptor table on a
                             * schedule the peer chooses. */
                            close(descriptor);
                        }
                    }
                }
                carried = CMSG_NXTHDR(&message, carried);
            }
            slot->filled += (int32_t)step;
            continue;
        }
        if (step == 0) {
            /* The peer closed. What has already arrived is still readable; the
             * channel stops being alive once it has been read out. */
            slot->alive = 0;
            return;
        }
        if (errno == EINTR) {
            continue;
        }
        return;
    }
}

#else /* _WIN32 */

/* The name an endpoint is published under. A named pipe is the Windows
 * rendezvous with the same shape as a unix socket: one name, many connections,
 * and the platform cleaning it up when the owner exits. */
static int kira_ipc_pipe_name(const char *name, char *out, size_t size) {
    int written = _snprintf_s(out, size, _TRUNCATE, "\\\\.\\pipe\\kira-ipc-%s", name != 0 ? name : "");
    return written > 0;
}

/* Every handle is opened FILE_FLAG_OVERLAPPED and every read and write carries
 * an OVERLAPPED. That is the only supported way to do non-blocking pipe I/O on
 * Windows: PIPE_NOWAIT is a LAN Manager 2.0 compatibility mode that Microsoft
 * documents as not for this, and it drops data on a byte-mode pipe. */
static int kira_ipc_arm_event(struct kira_ipc_slot *slot) {
    if (slot->event != 0) {
        return 1;
    }
    /* Manual-reset and initially signalled, which is what GetOverlappedResult
     * expects of an event it is asked about before a read has been issued. */
    slot->event = CreateEventA(0, TRUE, TRUE, 0);
    return slot->event != 0;
}

kira_ipc_id kira_ipc_listen(const char *name) {
    kira_ipc_id existing = kira_ipc_published(name);
    if (existing != 0) {
        return existing;
    }
    char path[256];
    if (kira_ipc_pipe_name(name, path, sizeof(path)) == 0) {
        return 0;
    }
    kira_ipc_id id = kira_ipc_slot_claim(KIRA_IPC_ENDPOINT);
    if (id == 0) {
        return 0;
    }
    struct kira_ipc_slot *slot = kira_ipc_slot_at(id, KIRA_IPC_ENDPOINT);
    memcpy(slot->name, path, sizeof(path));
    slot->handle = INVALID_HANDLE_VALUE;
    if (kira_ipc_arm_event(slot) == 0) {
        memset(slot, 0, sizeof(*slot));
        return 0;
    }
    return id;
}

/* One pipe instance is kept waiting so a connecting peer always finds an
 * instance to attach to. Accepting hands that instance over and arms the next. */
static int kira_ipc_arm_instance(struct kira_ipc_slot *slot) {
    if (slot->handle != INVALID_HANDLE_VALUE) {
        return 1;
    }
    slot->handle = CreateNamedPipeA(
        slot->name,
        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        PIPE_UNLIMITED_INSTANCES,
        65536,
        65536,
        0,
        0);
    if (slot->handle == INVALID_HANDLE_VALUE) {
        return 0;
    }
    memset(&slot->overlapped, 0, sizeof(slot->overlapped));
    ResetEvent(slot->event);
    slot->overlapped.hEvent = slot->event;
    slot->connecting = 1;
    if (ConnectNamedPipe(slot->handle, &slot->overlapped) != 0) {
        return 1;
    }
    DWORD error = GetLastError();
    if (error == ERROR_IO_PENDING) {
        return 1;
    }
    if (error == ERROR_PIPE_CONNECTED) {
        /* A peer connected between the create and the connect. The event is not
         * signalled in that case, so it is signalled here: the accept below asks
         * one question of one place either way. */
        SetEvent(slot->event);
        return 1;
    }
    CloseHandle(slot->handle);
    slot->handle = INVALID_HANDLE_VALUE;
    slot->connecting = 0;
    return 0;
}

kira_ipc_id kira_ipc_accept(kira_ipc_id endpoint) {
    struct kira_ipc_slot *slot = kira_ipc_slot_at(endpoint, KIRA_IPC_ENDPOINT);
    if (slot == 0 || kira_ipc_arm_instance(slot) == 0) {
        return 0;
    }
    DWORD transferred = 0;
    /* FALSE: ask, never wait. An accept that blocked would be a frame the shell
     * did not draw. */
    if (GetOverlappedResult(slot->handle, &slot->overlapped, &transferred, FALSE) == 0) {
        return 0;
    }
    kira_ipc_id id = kira_ipc_slot_claim(KIRA_IPC_CHANNEL);
    if (id == 0) {
        return 0;
    }
    struct kira_ipc_slot *channel = kira_ipc_slot_at(id, KIRA_IPC_CHANNEL);
    channel->handle = slot->handle;
    if (kira_ipc_arm_event(channel) == 0) {
        CloseHandle(channel->handle);
        memset(channel, 0, sizeof(*channel));
        return 0;
    }
    /* The instance has been handed over. The endpoint arms a fresh one on its
     * next accept, so the name always has an instance waiting on it. */
    slot->handle = INVALID_HANDLE_VALUE;
    slot->connecting = 0;
    return id;
}

kira_ipc_id kira_ipc_connect(const char *name) {
    char path[256];
    if (kira_ipc_pipe_name(name, path, sizeof(path)) == 0) {
        return 0;
    }
    HANDLE handle = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, 0,
                                OPEN_EXISTING, FILE_FLAG_OVERLAPPED, 0);
    if (handle == INVALID_HANDLE_VALUE) {
        return 0;
    }
    kira_ipc_id id = kira_ipc_slot_claim(KIRA_IPC_CHANNEL);
    if (id == 0) {
        CloseHandle(handle);
        return 0;
    }
    struct kira_ipc_slot *slot = kira_ipc_slot_at(id, KIRA_IPC_CHANNEL);
    slot->handle = handle;
    if (kira_ipc_arm_event(slot) == 0) {
        CloseHandle(handle);
        memset(slot, 0, sizeof(*slot));
        return 0;
    }
    return id;
}

/* Write every byte. A frame goes whole or the channel is finished: half a frame
 * in the peer's buffer is a stream whose framing is gone.
 *
 * The write waits for its own completion. That is not the same as blocking on
 * the peer: a pipe write completes when the bytes are in the pipe's buffer, and
 * a frame is far smaller than the 64K this side asked for. */
static int kira_ipc_write_all(struct kira_ipc_slot *slot, const uint8_t *bytes, int32_t count) {
    int32_t written = 0;
    while (written < count) {
        OVERLAPPED overlapped;
        memset(&overlapped, 0, sizeof(overlapped));
        HANDLE done = CreateEventA(0, TRUE, FALSE, 0);
        if (done == 0) {
            return 0;
        }
        overlapped.hEvent = done;
        DWORD step = 0;
        BOOL ok = WriteFile(slot->handle, bytes + written, (DWORD)(count - written), &step, &overlapped);
        if (ok == 0 && GetLastError() == ERROR_IO_PENDING) {
            ok = GetOverlappedResult(slot->handle, &overlapped, &step, TRUE);
        }
        CloseHandle(done);
        if (ok == 0 || step == 0) {
            return 0;
        }
        written += (int32_t)step;
    }
    return 1;
}

/* The process on the other end of this pipe.
 *
 * Needed because Windows transfers a handle by DUPLICATING it into the target
 * process, which means naming that process. A pipe knows both ends, so nothing
 * has to be told a process id it would then have to be trusted about. */
static HANDLE kira_ipc_peer_process(struct kira_ipc_slot *slot) {
    DWORD pid = 0;
    if (GetNamedPipeClientProcessId(slot->handle, &pid) == 0 || pid == 0) {
        if (GetNamedPipeServerProcessId(slot->handle, &pid) == 0 || pid == 0) {
            return 0;
        }
    }
    return OpenProcess(PROCESS_DUP_HANDLE, FALSE, pid);
}

static int32_t kira_ipc_send_frame(kira_ipc_id channel, const uint8_t *bytes, int32_t count, int carries, int64_t handle) {
    struct kira_ipc_slot *slot = kira_ipc_slot_at(channel, KIRA_IPC_CHANNEL);
    if (slot == 0 || slot->alive == 0 || count < 0 || count > KIRA_IPC_MAX_FRAME) {
        return 0;
    }
    int64_t carried = 0;
    if (carries) {
        /* Duplicated into the peer BEFORE the frame goes, so the value in the
         * header is already the peer's own. Sending this process's value would
         * name whatever that number happens to be over there -- a live handle to
         * something else, which is worse than no handle at all. */
        HANDLE peer = kira_ipc_peer_process(slot);
        if (peer == 0) {
            return 0;
        }
        HANDLE duplicate = 0;
        BOOL ok = DuplicateHandle(GetCurrentProcess(), (HANDLE)(intptr_t)handle,
                                  peer, &duplicate, 0, FALSE, DUPLICATE_SAME_ACCESS);
        CloseHandle(peer);
        if (ok == 0) {
            return 0;
        }
        carried = (int64_t)(intptr_t)duplicate;
    }
    uint8_t header[KIRA_IPC_HEADER];
    kira_ipc_write_header(header, count, carries, carried);
    if (kira_ipc_write_all(slot, header, (int32_t)sizeof(header)) == 0) {
        slot->alive = 0;
        return 0;
    }
    if (count > 0 && kira_ipc_write_all(slot, bytes, count) == 0) {
        slot->alive = 0;
        return 0;
    }
    return 1;
}

int32_t kira_ipc_send(kira_ipc_id channel, const uint8_t *bytes, int32_t count) {
    return kira_ipc_send_frame(channel, bytes, count, 0, 0);
}

int32_t kira_ipc_send_handle(kira_ipc_id channel, const uint8_t *bytes, int32_t count, int64_t handle) {
    return kira_ipc_send_frame(channel, bytes, count, 1, handle);
}

/* Collect whatever the pipe has, keeping exactly one read in flight. */
static void kira_ipc_fill(struct kira_ipc_slot *slot) {
    while (1) {
        if (slot->pending == 0) {
            memset(&slot->overlapped, 0, sizeof(slot->overlapped));
            ResetEvent(slot->event);
            slot->overlapped.hEvent = slot->event;
            DWORD step = 0;
            if (ReadFile(slot->handle, slot->chunk, sizeof(slot->chunk), &step, &slot->overlapped) == 0) {
                DWORD error = GetLastError();
                if (error == ERROR_IO_PENDING) {
                    slot->pending = 1;
                    return;
                }
                if (error == ERROR_BROKEN_PIPE) {
                    slot->alive = 0;
                }
                return;
            }
            slot->pending = 1;
        }
        DWORD transferred = 0;
        if (GetOverlappedResult(slot->handle, &slot->overlapped, &transferred, FALSE) == 0) {
            DWORD error = GetLastError();
            if (error == ERROR_IO_INCOMPLETE) {
                /* Still in flight. It stays in flight across calls, which is why
                 * `pending` is on the slot and not a local. */
                return;
            }
            slot->pending = 0;
            if (error == ERROR_BROKEN_PIPE) {
                slot->alive = 0;
            }
            return;
        }
        slot->pending = 0;
        if (transferred == 0) {
            return;
        }
        if (kira_ipc_reserve(slot, (int32_t)transferred) == 0) {
            return;
        }
        memcpy(slot->buffer + slot->filled, slot->chunk, (size_t)transferred);
        slot->filled += (int32_t)transferred;
    }
}

#endif

int32_t kira_ipc_poll(kira_ipc_id channel) {
    struct kira_ipc_slot *slot = kira_ipc_slot_at(channel, KIRA_IPC_CHANNEL);
    if (slot == 0) {
        return 0;
    }
    kira_ipc_fill(slot);
    return kira_ipc_front_frame(slot);
}

uint64_t kira_ipc_frame_word(kira_ipc_id channel, int32_t index) {
    struct kira_ipc_slot *slot = kira_ipc_slot_at(channel, KIRA_IPC_CHANNEL);
    if (slot == 0 || index < 0) {
        return 0;
    }
    int32_t length = kira_ipc_front_frame(slot);
    if (length == 0) {
        return 0;
    }
    uint64_t word = 0;
    for (int32_t step = 0; step < 8; step += 1) {
        int32_t offset = index * 8 + step;
        if (offset >= length) {
            break;
        }
        /* The frame's bytes start after its 4-byte length. Reading past the end
         * contributes nothing rather than trapping, so the tail word of a frame
         * whose length is not a multiple of eight is the bytes it has. */
        word |= (uint64_t)slot->buffer[KIRA_IPC_HEADER + offset] << (step * 8);
    }
    return word;
}

void kira_ipc_frame_release(kira_ipc_id channel) {
    struct kira_ipc_slot *slot = kira_ipc_slot_at(channel, KIRA_IPC_CHANNEL);
    if (slot == 0) {
        return;
    }
    int32_t length = kira_ipc_front_frame(slot);
    if (length == 0) {
        return;
    }
    /* An unclaimed handle is closed rather than leaked: a receiver that did not
     * understand the message must not slowly exhaust its descriptor table. */
    if (kira_ipc_front_carries_handle(slot) && slot->frontHandleTaken == 0) {
        int64_t orphan = kira_ipc_handle_pop(slot);
        if (orphan != 0) {
#if defined(_WIN32)
            CloseHandle((HANDLE)(intptr_t)orphan);
#else
            close((int)orphan);
#endif
        }
    }
    slot->frontHandleTaken = 0;
    int32_t consumed = KIRA_IPC_HEADER + length;
    /* What is left is the next frame and whatever followed it. Moving it to the
     * front keeps the frame a decoder reads at a known offset, which is what
     * lets `kira_ipc_frame_word` index from zero. */
    memmove(slot->buffer, slot->buffer + consumed, (size_t)(slot->filled - consumed));
    slot->filled -= consumed;
}

int64_t kira_ipc_frame_handle(kira_ipc_id channel) {
    struct kira_ipc_slot *slot = kira_ipc_slot_at(channel, KIRA_IPC_CHANNEL);
    if (slot == 0) {
        return 0;
    }
    if (kira_ipc_front_frame(slot) == 0 && slot->filled < KIRA_IPC_HEADER) {
        return 0;
    }
    if (kira_ipc_front_carries_handle(slot) == 0 || slot->frontHandleTaken) {
        return 0;
    }
    slot->frontHandleTaken = 1;
#if defined(_WIN32)
    /* Already the peer's own value: the sender duplicated it into this process
     * before the frame went, so it is a handle here and needs no queue. */
    return kira_ipc_front_header_handle(slot);
#else
    /* The kernel installed a descriptor when the bytes arrived; the header's
     * value is meaningless on this platform. */
    return kira_ipc_handle_pop(slot);
#endif
}

int32_t kira_ipc_alive(kira_ipc_id channel) {
    struct kira_ipc_slot *slot = kira_ipc_slot_at(channel, KIRA_IPC_CHANNEL);
    if (slot == 0) {
        return 0;
    }
    /* A peer that has gone leaves what it already sent readable. The channel is
     * alive while there is anything left to read out of it. */
    if (slot->alive == 0 && kira_ipc_front_frame(slot) == 0) {
        return 0;
    }
    return 1;
}

void kira_ipc_close(kira_ipc_id channel) {
    if (channel <= 0 || channel > KIRA_IPC_SLOTS) {
        return;
    }
    struct kira_ipc_slot *slot = &kira_ipc_slots[channel - 1];
    if (slot->kind == KIRA_IPC_FREE) {
        return;
    }
#if defined(_WIN32)
    if (slot->handle != INVALID_HANDLE_VALUE && slot->handle != 0) {
        /* Cancel the read still in flight before the handle goes: it addresses
         * `slot->chunk`, and the slot is about to be cleared under it. */
        if (slot->pending) {
            CancelIo(slot->handle);
        }
        CloseHandle(slot->handle);
    }
    if (slot->event != 0) {
        CloseHandle(slot->event);
    }
#else
    if (slot->fd > 0) {
        close(slot->fd);
    }
#endif
    /* Anything handed over and never claimed goes with the channel. */
    for (int32_t index = 0; index < slot->handleCount; index += 1) {
#if defined(_WIN32)
        CloseHandle((HANDLE)(intptr_t)slot->handles[index]);
#else
        close((int)slot->handles[index]);
#endif
    }
    free(slot->buffer);
    memset(slot, 0, sizeof(*slot));
}
