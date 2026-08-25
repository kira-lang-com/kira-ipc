DO NOT create any "smoke test", "probe" and anything that isn't meant to either: showcase KiraIpc or be driven by a real human. Anything called "smoke" or "probe" is outright **banned** from this repo.

This library has to be idiomatic, flawless, beautiful to read, kira native, using Kira's first party syntax. A single abstraction of the wrong shape, one stray helper, or one bad design and we FAILED.

The format is a CONTRACT between separately-installed programs. Two rules follow from that and neither is negotiable:

1. **A field is only ever appended.** Reordering or removing one renumbers every field after it, silently, in every peer built against the old declaration.
2. **Nothing traps on input.** These bytes come from another process. A malformed frame sets `failed` and decoding runs to completion; `ok()` is the one check a frame owes. A decoder that stopped the program would let any peer stop any other.

Everything that can be ordinary Kira is ordinary Kira. The native library is only what the language cannot spell — a Float's bits, a String from bytes, a socket — so the format itself runs identically on every backend and can be read.
