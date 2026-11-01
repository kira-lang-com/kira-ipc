# 26.10.4

**Project:** KiraIpc  
**Release week:** 2026-10-26 – 2026-11-01

## Highlights

The first source release of KiraIpc includes:

- `@Derive(IpcMessage)` generation of message encoders, decoders, and empty values.
- The KIP/1 wire format for scalar values, strings, byte arrays, arrays, nested messages, and enums.
- Forward-compatible handling of appended fields and an `ok()` result for malformed or truncated frames.

Field tags follow declaration order: removing or reordering fields breaks compatibility.
