# MsQuic ThirdParty

Microsoft's msquic — the QUIC stack used by Arch and Bevy on the server side.
Same library, same wire negotiation, so the Unreal server is comparable.

## Where to get the binaries

Download from the official releases page (pick the version matching what Arch
uses — currently 2.5+):

  https://github.com/microsoft/msquic/releases

You want the **schannel** Windows build (uses Windows' built-in TLS — no
OpenSSL dependency in msquic itself) and the **openssl** Linux build.

## Where to drop them

```
Source/ThirdParty/MsQuic/
├── MsQuic.Build.cs           (this is already here — committed)
├── include/
│   ├── msquic.h              (from the msquic release `include/` folder)
│   ├── msquic_posix.h
│   ├── msquic_winuser.h
│   └── quic_sal_stub.h       (Windows SAL stubs for non-MSVC; ship with release)
└── lib/
    ├── Win64/
    │   ├── msquic.lib        (import library for linking)
    │   └── msquic.dll        (runtime — auto-copied next to the .exe by Build.cs)
    └── Linux/
        └── libmsquic.so.2    (runtime + linkable; rename if release ships as
                               libmsquic.so.2.5.7 — the SONAME is .so.2)
```

## Verify the link

After dropping the binaries, regenerate Visual Studio project files and build
`UnrealvsHSServer Win64 Development`. If linking succeeds and the resulting
.exe launches with `[QuicServer] msquic initialized, ALPN=dgsvshs/2, port=7777`
in the log, you're good.

## License

msquic is MIT-licensed. Including its binaries in this project does not
impose copyleft on UnrealvsHS.
