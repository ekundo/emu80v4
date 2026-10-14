/*
 *  Автоматизация Emu80 для стенда: выход по счётчику кадров, снимки экрана и
 *  ввод по сценарию. Всё через переменные окружения, чтобы не трогать разбор
 *  командной строки:
 *
 *    EMU80_MAX_FRAME=2000            выйти после N кадров
 *    EMU80_AUTOSTART=1               нажать F12 самому, как только можно
 *    EMU80_KEYS="boot+900:SS+7"      нажатия: момент:клавиша
 *    EMU80_SAVE_FRAME=boot+1200,1900 сохранить эти кадры
 *    EMU80_FRAME_FILE=/tmp/shot      имена будут /tmp/shot-1200.ppm
 *    EMU80_QUIET_FRAMES=50           сколько кадров покоя считать концом загрузки
 *    EMU80_SAVE_EDD=1                перед выходом записать квазидиски обратно
 *                                    в файлы, указанные --edd/--edd2
 *    EMU80_TRACE=1                   печатать, что видит детектор загрузки
 *    EMU80_COV_LO=0100 EMU80_COV_HI=BFFF EMU80_COV_FILE=/tmp/cov.bin
 *    EMU80_COV_FROM=boot+1900 EMU80_COV_TO=boot+3500
 *                                    карта исполненных адресов за окно кадров:
 *                                    байт на адрес, потом diff двух прогонов
 *                                    показывает, где пути разошлись
 *
 *  Момент задаётся номером кадра или как `boot+N` -- через N кадров после того,
 *  как система догрузилась. Абсолютные кадры для этого не годятся: загрузка
 *  идёт разное время, а F12 до неё уводит машину в мусор.
 *
 *  Кадр берётся там, где готовая картинка уходит в окно (PalWindow::drawImage),
 *  поэтому снимок не зависит от того, работает ли вывод через OpenGL. Прогон
 *  ведём с SDL_VIDEODRIVER=dummy -- окно не открывается, снимки те же.
 *
 *  Формат PPM выбран нарочно: пишется в двадцать строк без внешних библиотек,
 *  а в PNG его переводит хоть sips, хоть что угодно.
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <map>

#include "../PalKeys.h"
#include "../PalWindow.h"
#include "../EmuCalls.h"
#include "../EmuAuto.h"

using namespace std;

namespace {

// Момент: либо кадр N, либо N кадров после того, как система догрузилась.
struct Moment { int offset; bool afterBoot; };

struct KeyEvent { Moment at; PalKeyCode key; bool press; };

bool inited = false;
int frameNo = 0;
int bootFrame = 0;              // кадр готовности системы (0 -- ещё не было)
uint32_t screenHash = 0;        // хеш последнего кадра
int screenFrame = 0;            // кадр, на котором картинка менялась в последний раз
int screenChanges = 0;          // сколько раз она вообще менялась
const int MIN_CHANGES = 5;      // меньше -- загрузка ещё и не начиналась
int quietFrames = 50;           // столько кадров покоя считаем концом загрузки
bool autostart = false;
bool saveEdd = false;           // записать квазидиски обратно перед выходом
int maxFrame = 0;
string framePath = "emu80-frame";
vector<Moment> saveFrames;
vector<Moment> dumpFrames;      // EMU80_DUMP_AT: снять слепок основного ОЗУ
int dumpLo = 0xB700, dumpHi = 0xBBFF;
int dumpBank = -1;              // EMU80_DUMP_BANK: A000-DFFF из этого банка КД (0..3)
vector<KeyEvent> keyEvents;

// Карта исполненных адресов. Пишем байт на адрес, а не последовательность:
// сравнивать два прогона куда проще по множеству адресов, чем по миллионам
// строк, а нам нужно ровно одно -- где путь на D: свернул не туда.
FILE* traceFile = nullptr;
int traceLo = 0, traceHi = -1, traceLeft = 0;
unsigned char* cov = nullptr;
int covLo = 0, covHi = -1;
Moment covFrom = {0, false}, covTo = {0, false};
string covPath;

// Наблюдение за записями в диапазон: кто и когда пишет в область, которая
// должна оставаться нетронутой (наш резидент под стеком, сам стек и т.п.).
//    EMU80_WATCH_LO=B700 EMU80_WATCH_HI=BBFF EMU80_WATCH_FILE=/tmp/w.txt
//    EMU80_WATCH_MAX=200000
// В строке: кадр, адрес, значение, PC команды, которая пишет.
FILE* watchFile = nullptr;
int watchLo = 0, watchHi = -1, watchLeft = 0;
int watchLo2 = 0, watchHi2 = -1;   // второй диапазон (буквы дисков панелей и т.п.)
uint16_t lastPc = 0;

// Самая глубокая точка стека за прогон: EMU80_SP_LO=B000 -- ниже неё не смотрим
// (до установки своего стека CO работает на чужом).
int spFloor = -1;          // включено, если задан EMU80_SP_LO
uint16_t spMin = 0xFFFF;
uint16_t spMinPc = 0;
int spMinFrame = 0;


bool inWindow()
{
    if (covTo.offset || covTo.afterBoot) {
        int from = covFrom.afterBoot ? (bootFrame ? bootFrame + covFrom.offset : 1 << 30) : covFrom.offset;
        int to = covTo.afterBoot ? (bootFrame ? bootFrame + covTo.offset : 1 << 30) : covTo.offset;
        return frameNo >= from && frameNo <= to;
    }
    return true;
}


bool trace()
{
    static bool on = getenv("EMU80_TRACE") != nullptr;
    return on;
}

PalKeyCode keyByName(const string& n)
{
    static const map<string, PalKeyCode> tbl = {
        {"F1", PK_F1}, {"F2", PK_F2}, {"F3", PK_F3}, {"F4", PK_F4},
        {"F5", PK_F5}, {"F6", PK_F6}, {"F7", PK_F7}, {"F8", PK_F8},
        {"F9", PK_F9}, {"F10", PK_F10}, {"F11", PK_F11}, {"F12", PK_F12},
        {"ENTER", PK_ENTER}, {"ESC", PK_ESC}, {"SPACE", PK_SPACE}, {"TAB", PK_TAB},
        {"BSP", PK_BSP}, {"UP", PK_UP}, {"DOWN", PK_DOWN},
        {"PGUP", PK_PGUP}, {"PGDN", PK_PGDN},   // на Векторе это СТР и ПС
        {"LEFT", PK_LEFT}, {"RIGHT", PK_RIGHT},
        {"SS", PK_LSHIFT},   // на Векторе это СС
        {"US", PK_LCTRL},    // и УС
        {"RUS", PK_RALT},    // РУС/ЛАТ -- привязка из VectorKbdLayout::translateKey
        {"0", PK_0}, {"1", PK_1}, {"2", PK_2}, {"3", PK_3}, {"4", PK_4},
        {"5", PK_5}, {"6", PK_6}, {"7", PK_7}, {"8", PK_8}, {"9", PK_9},
        {"A", PK_A}, {"B", PK_B}, {"C", PK_C}, {"D", PK_D}, {"E", PK_E},
        {"F", PK_F}, {"G", PK_G}, {"H", PK_H}, {"I", PK_I}, {"J", PK_J},
        {"K", PK_K}, {"L", PK_L}, {"M", PK_M}, {"N", PK_N}, {"O", PK_O},
        {"P", PK_P}, {"Q", PK_Q}, {"R", PK_R}, {"S", PK_S}, {"T", PK_T},
        {"U", PK_U}, {"V", PK_V}, {"W", PK_W}, {"X", PK_X}, {"Y", PK_Y},
        {"Z", PK_Z}, {"SEMICOLON", PK_SEMICOLON}, {"PERIOD", PK_PERIOD},
        {"COMMA", PK_COMMA}, {"MINUS", PK_MINUS}, {"SLASH", PK_SLASH},
        {"EQU", PK_EQU}, {"LBRACKET", PK_LBRACKET}, {"RBRACKET", PK_RBRACKET},
        {"BSLASH", PK_BSLASH}, {"APOSTROPHE", PK_APOSTROPHE}, {"TILDE", PK_TILDE},
    };
    auto it = tbl.find(n);
    return it == tbl.end() ? PK_NONE : it->second;
}

vector<string> split(const string& s, char sep)
{
    vector<string> out;
    size_t pos = 0;
    while (pos <= s.size()) {
        size_t next = s.find(sep, pos);
        if (next == string::npos) { out.push_back(s.substr(pos)); break; }
        out.push_back(s.substr(pos, next - pos));
        pos = next + 1;
    }
    return out;
}

Moment parseMoment(const string& s)      // "1500" или "boot+400"
{
    if (s.compare(0, 4, "boot") == 0) {
        size_t plus = s.find('+');
        return {plus == string::npos ? 0 : atoi(s.c_str() + plus + 1), true};
    }
    return {atoi(s.c_str()), false};
}

bool isNow(const Moment& m)
{
    if (!m.afterBoot)
        return m.offset == frameNo;
    return bootFrame && frameNo == bootFrame + m.offset;
}

// "boot+900:SS+7" -- нажать и через HOLD кадров отпустить.
// SS+7 значит: держать СС, нажать 7, отпустить обоих.
const int HOLD = 4;

void parseKeys(const string& spec)
{
    for (const string& item : split(spec, ',')) {
        size_t colon = item.find(':');
        if (colon == string::npos) continue;
        Moment at = parseMoment(item.substr(0, colon));
        for (const string& name : split(item.substr(colon + 1), '+')) {
            PalKeyCode k = keyByName(name);
            if (k == PK_NONE) {
                fprintf(stderr, "стенд: неизвестная клавиша «%s»\n", name.c_str());
                continue;
            }
            keyEvents.push_back({at, k, true});
            keyEvents.push_back({{at.offset + HOLD, at.afterBoot}, k, false});
        }
    }
}

void init()
{
    inited = true;
    if (const char* s = getenv("EMU80_MAX_FRAME")) maxFrame = atoi(s);
    if (const char* s = getenv("EMU80_FRAME_FILE")) framePath = s;
    if (const char* s = getenv("EMU80_QUIET_FRAMES")) quietFrames = atoi(s);
    if (const char* s = getenv("EMU80_SAVE_FRAME"))
        for (const string& f : split(s, ',')) saveFrames.push_back(parseMoment(f));
    if (const char* s = getenv("EMU80_DUMP_AT"))
        for (const string& f : split(s, ',')) dumpFrames.push_back(parseMoment(f));
    if (const char* s = getenv("EMU80_DUMP_BANK")) dumpBank = atoi(s);
    if (const char* s = getenv("EMU80_DUMP_LO")) dumpLo = strtol(s, nullptr, 16);
    if (const char* s = getenv("EMU80_DUMP_HI")) dumpHi = strtol(s, nullptr, 16);
    if (const char* s = getenv("EMU80_SP_LO")) spFloor = strtol(s, nullptr, 16);
    if (const char* s = getenv("EMU80_KEYS")) parseKeys(s);
    autostart = getenv("EMU80_AUTOSTART") != nullptr;
    saveEdd = getenv("EMU80_SAVE_EDD") != nullptr;
    if (const char* f = getenv("EMU80_COV_FROM")) covFrom = parseMoment(f);
    if (const char* t = getenv("EMU80_COV_TO")) covTo = parseMoment(t);
    if (const char* s = getenv("EMU80_TRACE_FILE")) {
        traceLo = strtol(getenv("EMU80_TRACE_LO") ? getenv("EMU80_TRACE_LO") : "0000", nullptr, 16);
        traceHi = strtol(getenv("EMU80_TRACE_HI") ? getenv("EMU80_TRACE_HI") : "FFFF", nullptr, 16);
        traceLeft = getenv("EMU80_TRACE_MAX") ? atoi(getenv("EMU80_TRACE_MAX")) : 200000;
        traceFile = fopen(s, "w");
        fprintf(stderr, "стенд: трасса %04X..%04X -> %s\n", traceLo, traceHi, s);
    }
    if (const char* s = getenv("EMU80_WATCH_FILE")) {
        watchLo = strtol(getenv("EMU80_WATCH_LO") ? getenv("EMU80_WATCH_LO") : "B700", nullptr, 16);
        watchHi = strtol(getenv("EMU80_WATCH_HI") ? getenv("EMU80_WATCH_HI") : "BBFF", nullptr, 16);
        watchLeft = getenv("EMU80_WATCH_MAX") ? atoi(getenv("EMU80_WATCH_MAX")) : 500000;
        if (getenv("EMU80_WATCH_LO2")) {
            watchLo2 = strtol(getenv("EMU80_WATCH_LO2"), nullptr, 16);
            watchHi2 = strtol(getenv("EMU80_WATCH_HI2") ? getenv("EMU80_WATCH_HI2") : "FFFF", nullptr, 16);
        }
        watchFile = fopen(s, "w");
        fprintf(stderr, "стенд: запись в %04X..%04X -> %s\n", watchLo, watchHi, s);
    }
    if (const char* s = getenv("EMU80_COV_FILE")) {
        covPath = s;
        covLo = strtol(getenv("EMU80_COV_LO") ? getenv("EMU80_COV_LO") : "0100", nullptr, 16);
        covHi = strtol(getenv("EMU80_COV_HI") ? getenv("EMU80_COV_HI") : "BFFF", nullptr, 16);
        cov = new unsigned char[covHi - covLo + 1]();
        fprintf(stderr, "стенд: карта адресов %04X..%04X -> %s\n", covLo, covHi, s);
    }
    if (maxFrame || !saveFrames.empty() || !keyEvents.empty() || autostart)
        fprintf(stderr, "стенд: кадров %d, снимков %zu, нажатий %zu, автостарт %s\n",
                maxFrame, saveFrames.size(), keyEvents.size() / 2, autostart ? "да" : "нет");
}

void saveFrame(uint32_t* pixels, int w, int h)
{
    char name[512];
    snprintf(name, sizeof name, "%s-%d.ppm", framePath.c_str(), frameNo);
    FILE* f = fopen(name, "wb");
    if (!f) { fprintf(stderr, "стенд: не пишется %s\n", name); return; }
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int i = 0; i < w * h; ++i) {
        uint32_t p = pixels[i];
        uint8_t rgb[3] = {uint8_t(p >> 16), uint8_t(p >> 8), uint8_t(p)};
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    fprintf(stderr, "стенд: кадр %d -> %s\n", frameNo, name);
}

// Конец загрузки: картинка замерла. Пока начальный загрузчик перекладывает
// систему из квазидиска в память, он рисует сетку прогресса снизу вверх;
// перестала расти -- значит, он дочитал и ждёт F12.
//
// Верхнюю пятую часть экрана не смотрим: там значок диска и содержимое стека,
// они меняются всё время и покоя не будет никогда. Диод РУС/ЛАТ, по которому
// автостартует v06x, для этого ПЗУ не годится -- за всю загрузку он дёргается
// один раз.
void watchScreen(uint32_t* pixels, int w, int h)
{
    if (!pixels)
        return;

    uint32_t hash = 2166136261u;
    for (int i = w * (h / 5); i < w * h; i += 7)
        hash = (hash ^ pixels[i]) * 16777619u;
    if (hash != screenHash) {
        screenHash = hash;
        screenFrame = frameNo;
        ++screenChanges;
    }

    if (trace() && frameNo % 100 == 0)
        fprintf(stderr, "стенд: кадр %d, хеш %08X, изменений %d, покой %d\n",
                frameNo, screenHash, screenChanges, frameNo - screenFrame);

    if (!bootFrame && screenChanges > MIN_CHANGES && frameNo - screenFrame == quietFrames) {
        bootFrame = frameNo;
        fprintf(stderr, "стенд: система загрузилась на кадре %d\n", frameNo);
    }
}

} // namespace


void autoOnFrame(PalWindow* wnd, uint32_t* pixels, int w, int h)
{
    if (!inited) init();
    ++frameNo;

    watchScreen(pixels, w, h);

    // Автостарт: F12 -- это сброс с отключённым ПЗУ, то есть запуск того, что
    // загрузчик положил в память.
    if (autostart && bootFrame) {
        if (frameNo == bootFrame + 1)
            emuKeyboard(wnd, PK_F12, true, 0);
        else if (frameNo == bootFrame + 1 + HOLD)
            emuKeyboard(wnd, PK_F12, false, 0);
    }

    for (const KeyEvent& e : keyEvents)
        if (isNow(e.at))
            emuKeyboard(wnd, e.key, e.press, 0);

    for (const Moment& m : saveFrames)
        if (isNow(m) && pixels)
            saveFrame(pixels, w, h);

    for (const Moment& m : dumpFrames)
        if (isNow(m)) {
            char name[512];
            snprintf(name, sizeof name, "%s-mem-%d.bin", framePath.c_str(), frameNo);
            FILE* f = fopen(name, "wb");
            if (f) {
                for (int a = dumpLo; a <= dumpHi; ++a) {
                    int b = (dumpBank >= 0 && a >= 0xA000 && a <= 0xDFFF)
                                ? autoPeekBank(a, dumpBank)
                                : autoPeekMain(a);
                    fputc(b < 0 ? 0 : b, f);
                }
                fclose(f);
                fprintf(stderr, "стенд: слепок ОЗУ %04X..%04X%s -> %s\n",
                        dumpLo, dumpHi,
                        dumpBank >= 0 ? " (банк КД)" : "", name);
            }
        }

    if (maxFrame && frameNo >= maxFrame) {
        if (saveEdd) {
            emuSysReq(wnd, SR_SAVERAMDISK);
            emuSysReq(wnd, SR_SAVERAMDISK2);
            fprintf(stderr, "стенд: квазидиски записаны обратно\n");
        }
        if (traceFile) fflush(traceFile);
        if (watchFile) fflush(watchFile);
        if (cov) {
            FILE* f = fopen(covPath.c_str(), "wb");
            if (f) {
                fwrite(cov, 1, covHi - covLo + 1, f);
                fclose(f);
                fprintf(stderr, "стенд: карта адресов записана\n");
            }
        }
        if (spFloor >= 0)
            fprintf(stderr, "стенд: самый низкий SP %04X (кадр %d, PC %04X)\n",
                    spMin, spMinFrame, spMinPc);
        fprintf(stderr, "стенд: отработано %d кадров, выходим\n", frameNo);
        fflush(stderr);
        exit(0);
    }
}


void autoWrite2(uint16_t addr, uint8_t value, int dest)
{
    if (watchFile && watchLeft > 0 && inWindow() &&
        ((addr >= watchLo && addr <= watchHi) || (addr >= watchLo2 && addr <= watchHi2))) {
        fprintf(watchFile, "%d %04X %02X %04X %d\n", frameNo, addr, value, lastPc, dest);
        if (--watchLeft == 0)
            fflush(watchFile);
    }
}


void autoSp(uint16_t sp, uint16_t pc)
{
    if (spFloor >= 0 && sp < spMin && sp >= spFloor && inWindow()) {
        spMin = sp; spMinPc = pc; spMinFrame = frameNo;
    }
}


void autoTrace(uint16_t pc, uint16_t af, uint16_t bc, uint16_t de, uint16_t hl, uint16_t sp)
{
    lastPc = pc;
    if (cov && pc >= covLo && pc <= covHi && inWindow())
        cov[pc - covLo] = 1;
    if (traceFile && traceLeft > 0 && pc >= traceLo && pc <= traceHi && inWindow()) {
        int r0 = autoPeekMain(sp), r1 = autoPeekMain((sp + 1) & 0xFFFF);
        int ret = (r0 < 0 || r1 < 0) ? 0 : (r0 | (r1 << 8));
        fprintf(traceFile, "%04X A=%02X F=%02X BC=%04X DE=%04X HL=%04X SP=%04X RET=%04X\n",
                pc, af >> 8, af & 0xFF, bc, de, hl, sp, ret);
        if (--traceLeft == 0)
            fflush(traceFile);
    }
}
