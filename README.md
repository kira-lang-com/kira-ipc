# KiraIpc

Typed IPC for Kira programs. A message type is an ordinary declaration with one
annotation on it; its encoder, its decoder and its empty value are generated.

```kira
@Derive(IpcMessage)
struct OpenTab {
    var address: String
    var foreground: Bool
}

var out = IpcWriter {}
encode_OpenTab(OpenTab { address: "https://example.com", foreground: true }, out)
let call = port.request("harmony.browser", "tabs.open", out.bytes)
```

## Why not Foundation's `Serializable`

Foundation writes `Point{x=1;y=2}` and traps on anything it cannot read back.
That is right for a value a program wrote itself: the text is legible, and
malformed input means a bug worth stopping for.

None of it holds across a process boundary. The bytes come from another program,
built from another version of the same declaration, possibly mid-crash. So:

| | Foundation `Serializable` | KiraIpc `IpcMessage` |
| --- | --- | --- |
| A field appended by a newer peer | read as its neighbour, silently | skipped, by key |
| A field a newer reader added | missing, unreadable | takes its declared default |
| `"` `;` `}` in a String | out of contract, will not round-trip | length-prefixed, crosses intact |
| `Float` | refused on decode | eight bytes, bit-exact |
| Arrays | refused | counted regions |
| A truncated frame | traps, stopping the program | `ok()` is false, program continues |

## The format

`KIP/1`. Every field is a varint key — its tag and its wire shape — then a value
whose length the key alone is enough to find. Three shapes, which is all a
decoder needs to step over a field it has no name for:

```
Var      a LEB128 varint            every integer, Bool, enum tag
Fixed    eight bytes, little-endian a Float, and only a Float
Len      a varint count, then bytes String, [U8], arrays, nested messages
```

A field's tag is its position in the declaration, counting from one. **Appending
a field is safe in both directions.** Reordering or removing one is a breaking
change, because both renumber every field after them.

## What a field may be

```
Int, I8, I16, I32, I64, U8, U16, U32   a zigzag varint
U64                                    an unsigned varint, its full range
Bool                                   a varint
Float, F32, F64                        eight bytes, bit-exact
String                                 length-prefixed UTF-8, unescaped
[U8]                                   length-prefixed bytes, not an array
[T] of any of the above                a counted region
another @Derive(IpcMessage) type          a nested region
```

An enum derives too: its variant's position, then the payload when it has one.
A nested or element type must be written unqualified — the generated call is
`decode_` glued to the type's source text.

## The channel

```kira
let endpoint = ipcListen("harmony.browser")     // publish a name
var port = ipcPort(ipcConnect("harmony.browser")) // or connect to one

let call = port.request(service, method, payload)  // returns the call number
let delivery = port.pump()                          // one frame, or none
```

Nothing blocks. A port is pumped beside whatever else its process owns — a
window's frame, an engine's run loop — because a process here has no thread to
spare to sit on a socket.

A channel that will not connect is the **ordinary** answer, not an error: it is
what a shell gets when the component it asked for is not installed, and what it
gets when the component is installed but has not come up. Both are handled the
same way, so neither is worth distinguishing.

## Handles

A GPU surface cannot be described by a number. A handle naming a dmabuf, a shared
texture, or a shared memory object is meaningful only inside the process that
owns it — put its value in a message and the peer reads whatever that number
happens to be over there, which is a live handle to something else.

So a handle is transferred by the **channel**, and a message carries only an id
saying which transferred handle it means:

```kira
port.emitHandle(service, "surface.attached", payload, surfaceHandle)

let delivery = port.pump()
if delivery.handle != 0 {
    // this process's own handle to the same object
}
```

| | |
| --- | --- |
| POSIX | rides the socket as an `SCM_RIGHTS` control message; the kernel installs a new descriptor |
| Windows | duplicated into the peer's process before the frame goes, so the value in it is already the peer's |

Handles arrive in order with the frames that carried them. A frame released
without its handle being claimed has that handle **closed**, not leaked: a
receiver that does not understand a message must not slowly exhaust its
descriptor table.

## What is native, and why

Three things, in `NativeLibs/kira_ipc.h`, because Kira has no spelling for them:
a Float's bits, a String built from bytes, and a socket. Everything else — the
tags, the varints, the field skipping, the generated encoders — is ordinary
Kira, so it runs identically on every backend and can be read.

The bindings are generated from the header by `Autobind`, so a signature is
declared once.

Frames travel out in one call and come back **eight bytes at a time**: a user FFI
signature has no way to return a byte array. That is affordable because of what
crosses here — control messages of a few hundred bytes. Bulk pixels do not come
this way; a surface is shared, and what crosses is the handle naming it.

## Examples

```bash
kira run Examples/typed-messages
```

Round-trips a message with a nested struct, an array and an enum payload; shows a
truncated frame decoding to a value with `ok()` false; and reads one declaration
of a message with a **newer** build's copy of it, in both directions.

```bash
kira run Examples/typed-channel
```

A service and its caller over a real channel: publish, connect, accept, a typed
request one way and a typed answer the other, correlated by call number.
