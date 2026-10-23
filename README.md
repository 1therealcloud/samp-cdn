# sampCDN

**sampCDN** is a lightweight HTTP download plugin for **SA-MP 0.3.DL-R1** (Windows and Linux x86).

By default, SA-MP transfers custom DFF/TXD models through RakNet. sampCDN redirects these downloads to an HTTP server running alongside the game server, allowing transfers to be **up to 20× faster** than the default method, depending on the connection and files.

## How it works

- Reads registered custom models from SA-MP's `CArtwork` pool and serves their DFF/TXD files by CRC32.
- Redirects client downloads to HTTP, with RakNet as a fallback when a file is unavailable.
- Hooks model registration to make newly added models available immediately.
- Reuses CRC32 results in memory for repeated files, such as shared TXDs.
- Supports HTTP Keep-Alive and fast file transfers with `TransmitFile` (Windows) or `sendfile` (Linux).

## Build

**Windows**:

```bash
cmake -S . -B build -A Win32
cmake --build build --config Release
```

**Linux**:

```bash
cmake -S . -B build -DCMAKE_CXX_FLAGS=-m32 -DCMAKE_SHARED_LINKER_FLAGS=-m32
cmake --build build -j
```

## Configuration

The plugin creates an template `cdn.cfg` next to the DLL/SO if it is missing. All settings are optional:

```ini
# sampCDN configuration
# Use a public IP or domain for remote players.
public_url = http://localhost:7777
bind = 0.0.0.0
```

`public_url` is the address sent to clients. By default it uses `http://localhost:<server port>`; for public servers, set your public IP or domain. `bind` defaults to `0.0.0.0`.

The HTTP server uses the same port number as SA-MP (`7777` by default), but **TCP** instead of UDP. Public servers must allow TCP connections on that port.

> Supports the SA-MP 0.3.DL-R1 server binaries targeted by the plugin, not other SA-MP versions or open.mp.
