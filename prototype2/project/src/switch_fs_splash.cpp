// Credit shown before the game (SDK switch_splash.cpp).
extern "C" const char* const* RexSwitchSplashLines(void) {
  static const char* const lines[] = {
      "t.me/fspores",
      "PROTOTYPE 2 for Nintendo Switch - free build by fungalspores",
      "port by penettrator (github.com/penettrator) - thank you!",
      "not for sale",
      nullptr,
  };
  return lines;
}
extern "C" unsigned RexSwitchSplashMs(void) { return 3500; }
