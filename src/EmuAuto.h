#ifndef EMUAUTO_H
#define EMUAUTO_H

#include <cstdint>

class PalWindow;

// Стенд: инструментовка для прогонов без человека -- см. sdl/emuAuto.cpp.
// Слепки памяти для стенда живут в Vector.cpp и есть в любой сборке.
int autoPeekMain(uint16_t addr);
int autoPeekBank(uint16_t addr, int bank);

#ifdef PAL_SDL
void autoOnFrame(PalWindow* wnd, uint32_t* pixels, int w, int h);
void autoTrace(uint16_t pc, uint16_t af, uint16_t bc, uint16_t de, uint16_t hl, uint16_t sp);
void autoWrite2(uint16_t addr, uint8_t value, int dest);
void autoSp(uint16_t sp, uint16_t pc);
#else
// sdl/emuAuto.cpp собирается только в SDL-сборке; в остальных вызовы из ядра пустые
inline void autoOnFrame(PalWindow*, uint32_t*, int, int) {}
inline void autoTrace(uint16_t, uint16_t, uint16_t, uint16_t, uint16_t, uint16_t) {}
inline void autoWrite2(uint16_t, uint8_t, int) {}
inline void autoSp(uint16_t, uint16_t) {}
#endif

#endif
