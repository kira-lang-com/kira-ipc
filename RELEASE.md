# 26.11.1

**Project:** KiraIpc  
**Release week:** 2026-11-02 – 2026-11-08

## Highlights

Native IPC and wire-conversion calls now have explicit `unsafe` boundaries, without exposing unsafe operations to KiraIPC callers or changing the KIP/1 wire format. The package passes the new compiler's check, native library build, and all 14 source-level tests. The wire round-trip test now verifies exact encoding bytes and captures consuming reads before making assertions.
