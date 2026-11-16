/*
 *  Emu80 v. 4.x
 *  © Viktor Pykhonin <pyk@mail.ru>, 2016-2025
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

// SoundMixer.cpp
// Реализация класса звукового микшера и базовых классов источника звука

#include <algorithm>

#include "Globals.h"
#include "Emulation.h"

#include "Pal.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "SoundMixer.h"
#include "Cpu.h"

using namespace std;

// Сумма -- в int16 через мягкий потолок, как загиб суммы на плате ComboLite: до
// 0,75 шкалы линейно, выше наклон 1/4, у края срез. Прежде лишнее переполняло
// int16 и перескакивало на другой край. Громкость 7 (сумма без знака) -- как была
int SoundMixer::limit(int x)
{
    if (m_volume >= 7)
        return x;
    const int knee = 24575;
    int a = x < 0 ? -x : x;
    if (a > knee)
        a = knee + (a - knee) / 4;
    if (a > 32767)
        a = 32767;
    return x < 0 ? -a : a;
}


// EMU80_WAV=путь: отладочная запись выхода микшера (16 бит, стерео) и при
// заглушенном звуке -- прогоны без окна стартуют заглушенными
static FILE* s_wav = nullptr;
static uint32_t s_wavN = 0;
static void wavHeader()
{
    uint32_t rate = g_emulation->getSampleRate(), data = s_wavN * 4, riff = 36 + data, br = rate * 4;
    uint8_t h[44] = {'R','I','F','F',0,0,0,0,'W','A','V','E','f','m','t',' ',16,0,0,0,1,0,2,0,
                     0,0,0,0,0,0,0,0,4,0,16,0,'d','a','t','a',0,0,0,0};
    memcpy(h + 4, &riff, 4); memcpy(h + 24, &rate, 4); memcpy(h + 28, &br, 4); memcpy(h + 40, &data, 4);
    long pos = ftell(s_wav);
    fseek(s_wav, 0, SEEK_SET); fwrite(h, 1, 44, s_wav); fseek(s_wav, pos < 44 ? 44 : pos, SEEK_SET);
    fflush(s_wav);
}
static void wavDump(int l, int r)
{
    static int state = -1;
    if (state < 0) {
        const char* p = getenv("EMU80_WAV");
        state = p && *p && (s_wav = fopen(p, "wb"));
        if (state) wavHeader();
    }
    if (!state) return;
    int16_t v[2] = {int16_t(l), int16_t(r)};
    fwrite(v, 2, 2, s_wav);
    if ((++s_wavN & 0x3fff) == 0) wavHeader();
}


// Вызывается 48000 (SAMPLE_RATE) раз в секунду для получения текущего сэмпла и его проигрывания
void SoundMixer::operate()
{
    int leftSample = 0;
    int rightSample = 0;
    for(auto it = m_soundSources.begin(); it != m_soundSources.end(); it++) {
        StereoSample sample = (*it)->getOutput();
        leftSample += m_volume < 7 ? sample.left : abs(sample.left - (*it)->getMinimumSampleValue());
        rightSample += m_volume < 7 ? sample.right : abs(sample.right - (*it)->getMinimumSampleValue());
    }

    leftSample = limit(leftSample >> m_sampleShift) + m_silenceLevel;
    rightSample = limit(rightSample >> m_sampleShift) + m_silenceLevel;
    wavDump(leftSample, rightSample);
    if (m_muted)
        leftSample = rightSample = m_silenceLevel;

    palPlaySample(leftSample, rightSample);

    m_curClock += m_ticksPerSample;

    m_error += m_ticksPerSampleRemainder;
    int delta = m_error / m_sampleRate;
    m_error -= delta * m_sampleRate;
    m_curClock += delta;
}


void SoundMixer::addSoundSource(SoundSource* snd)
{
    m_soundSources.push_back(snd);
}


void SoundMixer::removeSoundSource(SoundSource* snd)
{
    m_soundSources.remove(snd);
}


void SoundMixer::setFrequency(int64_t freq)
{
    m_sampleRate = g_emulation->getSampleRate();
    m_ticksPerSample = freq / m_sampleRate;
    m_ticksPerSampleRemainder = freq % m_sampleRate;
}


void SoundMixer::toggleMute()
{
    m_muted = !m_muted;
}


void SoundMixer::setVolume(int volume)
{
    if (volume < 1 || volume > 7)
        return;

    m_volume = volume;
    m_sampleShift = 7 - volume;

    if (volume <= 6)
        m_silenceLevel = 0;
    else
        m_silenceLevel = -32768;
}


int SoundMixer::getVolume()
{
    return m_volume;
}


SoundSource::SoundSource()
{
    g_emulation->getSoundMixer()->addSoundSource(this);
}


SoundSource::~SoundSource()
{
    g_emulation->getSoundMixer()->removeSoundSource(this);
}


void SoundSource::setVolume(int volume)
{
    m_volume = volume;
    updateAmpFactor();
}


void SoundSource::setNegative(bool negative)
{
    m_negative = negative;
    updateAmpFactor();
}


void SoundSource::setMuted(bool muted)
{
    m_muted = muted;
    updateAmpFactor();
}


void SoundSource::updateAmpFactor()
{
    m_ampFactor = m_muted ? 0 : m_negative ? -m_volume : m_volume;
}


StereoSample SoundSource::getOutput()
{
    StereoSample s = getSample();
    if (!m_dcBlock)
        return s;
    if (!m_dcPrimed) {
        // исходный уровень -- первый отсчет: иначе удар на запуске
        m_dcPrimed = true;
        m_dcXL = s.left;
        m_dcXR = s.right;
    }
    double a = 1.0 - 2 * 3.14159265 * 4 / g_emulation->getSampleRate();
    m_dcYL = s.left - m_dcXL + a * m_dcYL;
    m_dcXL = s.left;
    m_dcYR = s.right - m_dcXR + a * m_dcYR;
    m_dcXR = s.right;
    return {int(m_dcYL), int(m_dcYR)};
}


StereoSample SoundSource::getSample()
{
    int val = calcValue() * m_ampFactor / 100;
    return {val, val};
}


bool SoundSource::setProperty(const std::string& propertyName, const EmuValuesList& values)
{
    if (EmuObject::setProperty(propertyName, values))
        return true;

    if (propertyName == "muted") {
        if (values[0].asString() == "yes" || values[0].asString() == "no") {
            setMuted(values[0].asString() == "yes");
            return true;
        }
    } else if (propertyName == "volume") {
        setVolume(values[0].asInt());
        return true;
    } else if (propertyName == "dcBlock") {
        if (values[0].asString() == "yes" || values[0].asString() == "no") {
            setDcBlock(values[0].asString() == "yes");
            return true;
        }
    } else if (propertyName == "polarity") {
        if (values[0].asString() == "positive" || values[0].asString() == "negative") {
            setNegative(values[0].asString() == "negative");
            return true;
        }
    }
    return false;
}


std::string SoundSource::getPropertyStringValue(const std::string& propertyName)
{
    string res;

    res = EmuObject::getPropertyStringValue(propertyName);
    if (res != "")
        return res;

    if (propertyName == "muted")
        return m_muted ? "yes" : "no";

    return "";
}


// A change timed by the CPU may be ahead of the emulation clock (by the part of
// the instruction already executed), but never earlier than the previous change
uint64_t GeneralSoundSource::getChangeClock()
{
    return m_cpu ? m_cpu->getCurMCycleClock() : g_emulation->getCurClock();
}


void GeneralSoundSource::setValue(int value)
{
    updateStats(getChangeClock());
    m_prevValue = m_curValue;
    m_curValue = value;
}


void GeneralSoundSource::setFlip(int)
{
    updateStats(getChangeClock());
    m_prevValue = m_curValue;
    m_curValue ^= 1;
}


// Обновляет внутренние счетчики, вызывается перед установкой нового значения либо перед получением текущего
void GeneralSoundSource::updateStats(uint64_t curClock)
{
    if (curClock <= prevClock)
        return;

    if (m_curValue) {
        int clocks = curClock - prevClock;
        sumVal += clocks;
    }

    prevClock = curClock;
}

// Получение текущего значения
int GeneralSoundSource::calcValue()
{
    uint64_t curClock = g_emulation->getCurClock();
    updateStats(curClock);

    // The last change may be ahead of the end of this sample: the time from the end
    // of the sample to the change belongs to the next one
    int carry = 0;
    if (prevClock > curClock && m_prevValue)
        carry = min(int(prevClock - curClock), sumVal);
    sumVal -= carry;

    int res = 0;

    uint64_t ticks = curClock - initClock;
    if (ticks)
            res = sumVal * MAX_SND_AMP / ticks;
    sumVal = carry;
    initClock = curClock;

    return res;
}


bool GeneralSoundSource::setProperty(const string& propertyName, const EmuValuesList& values)
{
    if (SoundSource::setProperty(propertyName, values))
        return true;

    if (propertyName == "cpu") {
        m_cpu = static_cast<Cpu*>(g_emulation->findObject(values[0].asString()));
        return true;
    }

    return false;
}


void GeneralSoundSource::initConnections()
{
    SoundSource::initConnections();

    REG_INPUT("input", GeneralSoundSource::setValue);
    REG_INPUT("flip", GeneralSoundSource::setFlip);
}
