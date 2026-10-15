// Credit shown before the game (SDK switch_splash.cpp).
extern "C" const char* const* RexSwitchSplashLines(void) {
  static const char* const lines[] = {
      "t.me/fspores",
      "PROTOTYPE for Nintendo Switch - free port by fungalspores",
      "ReXGlue static recompilation - not for sale",
      nullptr,
  };
  return lines;
}
extern "C" unsigned RexSwitchSplashMs(void) { return 3000; }
