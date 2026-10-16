// Screenshots for checking frames for rendering bugs.
//
// 1. Lets the system Capture button work in the game (Album, readable over MTP).
//    On hardware (run14) the permission call returned 0 but Capture still failed.
// 2. Own capture through caps:sc (capsscCaptureJpegScreenShot, 10.0.0+) on L+R+ZR only (no
//    automatic captures: they would fill the SD card). The SDK input driver calls
//    RexSwitchScreenshotRequest on its polling thread: a thread of our own failed to start in
//    run16 (threadCreate 0xE001, the process is at its thread limit). JPEGs go to
//    sdmc:/switch/prototype2-nx/logs/screens/, result codes to screens/screens.log; agents copy
//    them together with the logs.
#include <switch.h>

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <sys/stat.h>

namespace {

constexpr const char* kScreensDir = "sdmc:/switch/prototype2-nx/logs/screens";

void Log(const char* text) {
  char path[128];
  std::snprintf(path, sizeof(path), "%s/screens.log", kScreensDir);
  if (FILE* f = std::fopen(path, "a")) {
    std::fprintf(f, "%s\n", text);
    std::fclose(f);
  }
}

}  // namespace

extern "C" void P2CollisionSnapshotRequest();
extern "C" void P2NativeVisualSnapshotRequest();
extern "C" void RexSwitchScreenshotRequest() {
  P2CollisionSnapshotRequest();
  P2NativeVisualSnapshotRequest();
  static void* jpeg = nullptr;
  static bool initialized = false;
  char line[160];
  if (!initialized) {
    initialized = true;
    Result rc = capsscInitialize();
    std::snprintf(line, sizeof(line), "caps:sc init 0x%08X", rc);
    Log(line);
    if (R_SUCCEEDED(rc)) jpeg = std::malloc(CAPSSC_JPEG_BUFFER_SIZE);
  }
  if (!jpeg) return;
  std::time_t now = std::time(nullptr);
  std::tm local = *std::localtime(&now);
  char stamp[32];
  std::strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", &local);
  // The application layers first: the user-screenshot stack may exclude this layer
  // (system Capture failed on hardware).
  static const ViLayerStack kStacks[] = {ViLayerStack_ApplicationForDebug,
                                         ViLayerStack_Screenshot, ViLayerStack_Default};
  for (ViLayerStack stack : kStacks) {
    u64 size = 0;
    Result rc = capsscCaptureJpegScreenShot(&size, jpeg, CAPSSC_JPEG_BUFFER_SIZE, stack,
                                            100000000);
    if (R_FAILED(rc) || !size) {
      std::snprintf(line, sizeof(line), "%s stack %d failed 0x%08X", stamp, int(stack), rc);
      Log(line);
      continue;
    }
    char path[160];
    std::snprintf(path, sizeof(path), "%s/%s.jpg", kScreensDir, stamp);
    FILE* f = std::fopen(path, "wb");
    bool written = f && std::fwrite(jpeg, 1, size, f) == size;
    if (f) std::fclose(f);
    std::snprintf(line, sizeof(line), "%s stack %d %llu bytes %s", stamp, int(stack),
                  (unsigned long long)size, written ? "saved" : "write failed");
    Log(line);
    return;
  }
}

unsigned int prototype2_EnableSystemScreenshots() {
  mkdir("sdmc:/switch/prototype2-nx/logs", 0777);
  mkdir(kScreensDir, 0777);
  if (appletGetAppletType() != AppletType_Application) return 0xFFFFFFFFu;
  return appletSetScreenShotPermission(AppletScreenShotPermission_Enable);
}
