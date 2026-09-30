TEMPLATE = subdirs
CONFIG += ordered

framing.file = $$PWD/framing.pro
SUBDIRS += framing

# The round trip compiles the vendored codec and needs a Vulkan GPU at runtime.
# The codec needs nothing beyond Vulkan, so the same test guards the Linux path
# too; it passes on both the NVIDIA and the Intel driver here. Only the D3D11
# surface pool test stays Windows-only.
contains(QT_ARCH, x86_64) {
    win32 {
        roundtrip.file = $$PWD/roundtrip.pro
        SUBDIRS += roundtrip

        # The client's D3D11 surface pool and Vulkan decoder, without a host
        d3d11.file = $$PWD/d3d11.pro
        SUBDIRS += d3d11
    }

    unix:!macx {
        roundtrip.file = $$PWD/roundtrip.pro
        SUBDIRS += roundtrip
    }
}
