// Link stub for libSceAmpr, which PS5_PayloadSDK has no stub for. What it does: names the libSceAmpr exports the PS5
// page-table probe imports, so the native converter records them as imports of libSceAmpr.prx and the console's loader
// binds them at process start. Why: a title's sceKernelDlsym finds nothing on this console (PS5_Vulkan
// docs/HARDWARE_FINDINGS.md), so a linked import is the only way in. How: build_smoke.py --page-table compiles it into
// libSceAmpr.so (soname libSceAmpr.prx), as the template's link-title.sh does for libSceAgc. The bodies never run.
// Names checked against the export NIDs of libSceAmpr (docs/research.md, DNNDHH/PS5-3.20_Libs).
void sceAmprAmmGetVirtualAddressRanges(void) {}
