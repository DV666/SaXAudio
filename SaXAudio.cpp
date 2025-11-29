// MIT License
// 
// Copyright(c) 2025 SamsamTS
// 
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files(the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and /or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions :
// 
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
// 
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include "SaXAudio.h"
#include "Fader.h"
#include <vector>
#include <string>
#include <algorithm>
#include <psapi.h> 

#pragma comment(lib, "Psapi.lib")

namespace SaXAudio
{
#define GetEntry(data, map, id) nullptr; auto it_##data = map.find(id); if (it_##data != map.end()) data = &it_##data->second
#define CHAIN_REVERB 0
#define CHAIN_EQ 1
#define CHAIN_ECHO 2

#define MAX_VOICES 64
#define MAX_POOL_SIZE 100
#define MAX_UNUSED_BUFFERS 50

    static vector<AudioVoice*> g_inactiveVoices;
    static vector<AudioVoice*> g_trashBin; // Le Purgatoire (Zone de transit avant destruction)

    SaXAudio& SaXAudio::Instance = SaXAudio::getInstance();

    BOOL SaXAudio::Init()
    {
        if (m_XAudio) return true;

        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        switch (hr)
        {
        case S_OK: case S_FALSE: break;
        case RPC_E_CHANGED_MODE: hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED); if (SUCCEEDED(hr)) break;
        default: return false;
        }

        hr = XAudio2Create(&m_XAudio, 0, XAUDIO2_DEFAULT_PROCESSOR);
        if (FAILED(hr)) { m_XAudio = nullptr; return false; }

        IXAudio2MasteringVoice* masteringVoice;
        hr = m_XAudio->CreateMasteringVoice(&masteringVoice, 0, 48000);
        if (FAILED(hr)) { m_XAudio->Release(); m_XAudio = nullptr; return false; }

        m_masteringBus.voice = masteringVoice;
        hr = masteringVoice->GetChannelMask(&m_channelMask);
        if (FAILED(hr)) { m_XAudio->Release(); m_XAudio = nullptr; return false; }

        masteringVoice->GetVoiceDetails(&m_masterDetails);

        g_inactiveVoices.reserve(MAX_POOL_SIZE);
        g_trashBin.reserve(64); // Réserve plus large pour le purgatoire

        return true;
    }

    void DestroyVoicePhysical(AudioVoice* voice)
    {
        if (!voice) return;

        // ATTENTION : Cette fonction est bloquante.
        // Elle ne doit JAMAIS être appelée si un Mutex est verrouillé.

        if (voice->SourceVoice)
        {
            voice->SourceVoice->Stop();
            voice->SourceVoice->FlushSourceBuffers();
            voice->SourceVoice->SetEffectChain(nullptr);
            voice->SourceVoice->DestroyVoice(); // <--- C'est ici que ça freeze si on tient un lock
            voice->SourceVoice = nullptr;
        }

        for (int i = 0; i < 3; ++i)
        {
            if (voice->EffectData.descriptors[i].pEffect)
            {
                voice->EffectData.descriptors[i].pEffect->Release();
                voice->EffectData.descriptors[i].pEffect = nullptr;
            }
        }

        delete voice;
    }

    void SaXAudio::Release()
    {
        if (!m_XAudio) return;
        m_XAudio->StopEngine();

        for (AudioVoice* v : g_inactiveVoices) DestroyVoicePhysical(v);
        g_inactiveVoices.clear();

        for (AudioVoice* v : g_trashBin) DestroyVoicePhysical(v);
        g_trashBin.clear();

        while (!m_voicePool.empty()) { delete m_voicePool.front(); m_voicePool.pop(); }

        for (auto& it : m_voices) DestroyVoicePhysical(it.second);
        m_voices.clear();

        while (!m_bank.empty()) RemoveBankEntry(m_bank.begin()->first);

        for (auto& it : m_bufferPool) delete[] it.Data;
        m_bufferPool.clear();

        m_XAudio->Release(); m_XAudio = nullptr;
        m_masteringBus.voice = nullptr;
    }

    void SaXAudio::StopEngine() { if (m_XAudio) m_XAudio->StopEngine(); }
    void SaXAudio::StartEngine() { if (m_XAudio) m_XAudio->StartEngine(); }

    void SaXAudio::PauseAll(const FLOAT fade, const INT32 busID)
    {
        if (!m_XAudio) return;
        lock_guard<recursive_mutex> lock(m_voiceMutex);
        for (auto& it : m_voices) if (!it.second->IsProtected && (busID == 0 || it.second->BusID == busID)) it.second->Pause(fade);
    }
    void SaXAudio::ResumeAll(const FLOAT fade, const INT32 busID)
    {
        if (!m_XAudio) return;
        lock_guard<recursive_mutex> lock(m_voiceMutex);
        for (auto& it : m_voices) if (!it.second->IsProtected && (busID == 0 || it.second->BusID == busID)) it.second->Resume(fade);
    }
    void SaXAudio::StopAll(const FLOAT fade, const INT32 busID)
    {
        if (!m_XAudio) return;
        lock_guard<recursive_mutex> lock(m_voiceMutex);
        for (auto& it : m_voices) if (!it.second->IsProtected && (busID == 0 || it.second->BusID == busID)) it.second->Stop(fade);
    }
    void SaXAudio::Protect(const INT32 voiceID)
    {
        if (!m_XAudio) return;
        AudioVoice* voice = GetVoice(voiceID);
        if (voice) { voice->IsProtected = true; if (voice->IsPlaying) while (voice->Resume()); }
    }

    INT32 SaXAudio::AddBankEntry(const OnDecodedCallback callback)
    {
        if (!m_XAudio) return 0;
        lock_guard<recursive_mutex> lock(m_bankMutex);
        m_bank[m_bankCounter].onDecodedCallback = callback;
        return m_bankCounter++;
    }

    void SaXAudio::RemoveBankEntry(const INT32 bankID)
    {
        lock_guard<recursive_mutex> lock(m_bankMutex);

        BankData* data = GetEntry(data, m_bank, bankID);
        if (!data) return;

        data->autoRemove = true;
        data->disposed = true;

        if (m_XAudio)
        {
            lock_guard<recursive_mutex> voiceLock(m_voiceMutex);
            for (auto& it : m_voices)
            {
                if (it.second->BankID == bankID) return;
            }
        }

        m_bufferPool.push_back(data->buffer);
        if (data->onDecodedCallback) { (*data->onDecodedCallback)(bankID, data->Oggbuffer); data->onDecodedCallback = nullptr; }
        m_bank.erase(bankID);
    }

    void SaXAudio::AutoRemoveBank(const INT32 bankID)
    {
        if (!m_XAudio) return;
        lock_guard<recursive_mutex> lock(m_bankMutex);
        BankData* data = GetEntry(data, m_bank, bankID);
        if (!data) return;
        data->autoRemove = true;
    }

    INT32 SaXAudio::AddBus()
    {
        if (!m_XAudio) return 0;
        lock_guard<mutex> lock(m_busMutex);
        IXAudio2SubmixVoice* bus;
        HRESULT hr = m_XAudio->CreateSubmixVoice(&bus, m_masterDetails.InputChannels, m_masterDetails.InputSampleRate);
        if (FAILED(hr)) return 0;
        BusData* data = &m_buses[m_busCounter];
        data->voice = bus;
        data->effectChain = { 3, nullptr };
        for (int i = 0; i < 3; i++) data->descriptors[i].pEffect = nullptr;
        return m_busCounter++;
    }

    void SaXAudio::RemoveBus(const INT32 busID)
    {
        if (!m_XAudio) return;
        lock_guard<mutex> lock(m_busMutex);
        BusData* bus = GetEntry(bus, m_buses, busID);
        if (!bus) return;

        {
            lock_guard<recursive_mutex> vLock(m_voiceMutex);
            for (auto& it : m_voices) if (it.second->BusID == busID) it.second->Stop();
        }

        for (int i = 0; i < 3; ++i) if (bus->descriptors[i].pEffect) { bus->descriptors[i].pEffect->Release(); bus->descriptors[i].pEffect = nullptr; }
        bus->voice->DestroyVoice();
        m_buses.erase(busID);
    }

    BusData* SaXAudio::GetBus(const INT32 busID)
    {
        if (!m_XAudio) return nullptr;
        lock_guard<mutex> lock(m_busMutex);
        BusData* bus = GetEntry(bus, m_buses, busID);
        return bus;
    }

    static void OnFadeVolume(INT64 busID, UINT32 count, FLOAT* newValues, BOOL hasFinished)
    {
        BusData* bus = SaXAudio::Instance.GetBus((INT32)busID);
        if (!bus || !bus->voice) return;
        bus->voice->SetVolume(newValues[0]);
    }
    void SaXAudio::SetBusVolume(const INT32 busID, const FLOAT volume, const FLOAT fade)
    {
        if (!m_XAudio) return;
        BusData* bus = busID == 0 ? &m_masteringBus : GetBus(busID);
        if (!bus || !bus->voice) return;
        Fader::Instance.StopFade(bus->fadeID); bus->fadeID = 0;
        if (fade > 0)
        {
            FLOAT current = 1.0f; bus->voice->GetVolume(&current);
            bus->fadeID = Fader::Instance.StartFade(current, volume, fade, OnFadeVolume, busID);
        }
        else { bus->voice->SetVolume(volume); }
    }
    FLOAT SaXAudio::GetBusVolume(const INT32 busID)
    {
        if (!m_XAudio) return 0.0f;
        BusData* bus = busID == 0 ? &m_masteringBus : GetBus(busID);
        if (!bus || !bus->voice) return 0.0f;
        FLOAT volume; bus->voice->GetVolume(&volume); return volume;
    }

    Buffer SaXAudio::GetBuffer(UINT32 length)
    {
        lock_guard<recursive_mutex> lock(m_bankMutex);
        Buffer buffer;
        if (length == 0) return buffer;
        buffer.Size = 1024;
        while (buffer.Size < length) buffer.Size <<= 1;
        auto it = m_bufferPool.begin(); auto end = m_bufferPool.end(); auto candidate = end;
        while (it != end)
        {
            if (it->Size == buffer.Size) { candidate = it; break; }
            if (it->Size > buffer.Size && it->Size < buffer.Size * 4) { if (candidate == end || it->Size < candidate->Size) candidate = it; }
            it++;
        }
        if (candidate != end)
        {
            buffer = *candidate; m_bufferPool.erase(candidate); return buffer;
        }
        buffer.Data = new FLOAT[buffer.Size];
        return buffer;
    }
    void SaXAudio::ReturnBuffer(Buffer buffer)
    {
        lock_guard<recursive_mutex> bankLock(SaXAudio::Instance.m_bankMutex);
        m_bufferPool.push_back(buffer);
    }
    UINT32 SaXAudio::AddBankData(Buffer buffer, UINT32 channels, UINT32 sampleRate, UINT32 totalSamples)
    {
        if (!m_XAudio) return 0;
        lock_guard<recursive_mutex> bankLock(SaXAudio::Instance.m_bankMutex);
        BankData* data = &m_bank[m_bankCounter];
        data->buffer = buffer; data->channels = channels; data->sampleRate = sampleRate;
        data->totalSamples = totalSamples; data->decodedSamples = data->totalSamples;
        return m_bankCounter++;
    }
    BOOL SaXAudio::StartDecodeOgg(const INT32 bankID, const BYTE* buffer, const UINT32 length)
    {
        int error; stb_vorbis* vorbis = stb_vorbis_open_memory(buffer, length, &error, NULL);
        if (!vorbis) return FALSE;
        lock_guard<recursive_mutex> bankLock(m_bankMutex);
        BankData* data = GetEntry(data, m_bank, bankID);
        if (!data) return FALSE;
        data->Oggbuffer = buffer;
        stb_vorbis_info info = stb_vorbis_get_info(vorbis);
        data->channels = info.channels; data->sampleRate = info.sample_rate;
        data->totalSamples = stb_vorbis_stream_length_in_samples(vorbis);
        data->buffer = GetBuffer(data->totalSamples * data->channels);
        thread decode(DecodeOgg, bankID, vorbis);
        decode.detach();
        return TRUE;
    }

    AudioVoice* SaXAudio::CreateVoice(const INT32 bankID, const INT32 busID)
    {
        if (!m_XAudio) return nullptr;

        lock_guard<recursive_mutex> bankLock(m_bankMutex);
        lock_guard<mutex> busLock(m_busMutex);

        // VERROUILLAGE PRINCIPAL
        lock_guard<recursive_mutex> voiceLock(m_voiceMutex);

        BankData* data = GetEntry(data, m_bank, bankID);
        if (!data || data->disposed) return nullptr;

        UINT32 channels = data->channels;
        UINT32 sampleRate = data->sampleRate;

        // --- NETTOYAGE DU POOL (SANS DESTRUCTION PHYSIQUE) ---
        // Si le pool est plein, on déplace les vieux objets vers le purgatoire (g_trashBin).
        // On ne fait JAMAIS de delete ou DestroyVoice ici car on tient le lock !
        while (g_inactiveVoices.size() > MAX_POOL_SIZE)
        {
            AudioVoice* v = g_inactiveVoices.back();
            g_inactiveVoices.pop_back();
            g_trashBin.push_back(v); // Transfert vers la poubelle, à vider dans Update
        }

        AudioVoice* voice = nullptr;
        for (auto it = g_inactiveVoices.begin(); it != g_inactiveVoices.end(); ++it)
        {
            AudioVoice* candidate = *it;
            if (candidate->SourceVoice)
            {
                XAUDIO2_VOICE_DETAILS details;
                candidate->SourceVoice->GetVoiceDetails(&details);
                if (details.InputChannels == channels && details.InputSampleRate == sampleRate)
                {
                    voice = candidate;
                    *it = g_inactiveVoices.back();
                    g_inactiveVoices.pop_back();
                    break;
                }
            }
        }

        if (!voice) voice = new AudioVoice();
        voice->Reset();
        BusData* bus = GetEntry(bus, m_buses, busID);

        if (!voice->SourceVoice)
        {
            WAVEFORMATEX wfx = {};
            wfx.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
            wfx.nChannels = static_cast<WORD>(channels);
            wfx.nSamplesPerSec = static_cast<DWORD>(sampleRate);
            wfx.wBitsPerSample = 32;
            wfx.nBlockAlign = wfx.nChannels * wfx.wBitsPerSample / 8;
            wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;
            wfx.cbSize = 0;

            HRESULT hr;
            if (bus && bus->voice)
            {
                XAUDIO2_SEND_DESCRIPTOR sendDesc { 0, bus->voice };
                XAUDIO2_VOICE_SENDS sends { 1, &sendDesc };
                hr = m_XAudio->CreateSourceVoice(&voice->SourceVoice, &wfx, 0, XAUDIO2_MAX_FREQ_RATIO, voice, &sends, nullptr);
            }
            else
            {
                hr = m_XAudio->CreateSourceVoice(&voice->SourceVoice, &wfx, 0, XAUDIO2_MAX_FREQ_RATIO, voice, nullptr, nullptr);
            }
            if (FAILED(hr)) { delete voice; return nullptr; }
        }
        else
        {
            if (voice->BusID != busID)
            {
                XAUDIO2_VOICE_SENDS* pSendList = nullptr;
                XAUDIO2_VOICE_SENDS sends;
                XAUDIO2_SEND_DESCRIPTOR sendDesc;
                if (bus && bus->voice)
                {
                    sendDesc.Flags = 0; sendDesc.pOutputVoice = bus->voice;
                    sends.SendCount = 1; sends.pSends = &sendDesc; pSendList = &sends;
                }
                voice->SourceVoice->SetOutputVoices(pSendList);
            }
            voice->SourceVoice->FlushSourceBuffers();
            voice->SourceVoice->SetVolume(1.0f);
            voice->SourceVoice->SetFrequencyRatio(1.0f);

            voice->SourceVoice->DisableEffect(CHAIN_REVERB);
            voice->SourceVoice->DisableEffect(CHAIN_EQ);
            voice->SourceVoice->DisableEffect(CHAIN_ECHO);
        }

        voice->BankData = data;
        voice->BankID = bankID;
        voice->VoiceID = m_voiceCounter++;
        voice->BusID = bus ? busID : 0;

        voice->Buffer = {};
        voice->Buffer.AudioBytes = static_cast<UINT32>(sizeof(float) * data->totalSamples * data->channels);
        voice->Buffer.pAudioData = reinterpret_cast<const BYTE*>(data->buffer.Data);
        voice->Buffer.Flags = XAUDIO2_END_OF_STREAM;
        voice->SetOutputMatrix(0.0f);

        m_voices[voice->VoiceID] = voice;
        return voice;
    }

    void SaXAudio::RemoveVoice(const INT32 voiceID)
    {
        INT32 bankID = 0;
        AudioVoice* voice = nullptr;
        bool shouldCheckRemoval = false;

        {
            // Étape 1 : retirer proprement la voix
            std::lock_guard<std::recursive_mutex> lock(m_voiceMutex);
            auto it = m_voices.find(voiceID);
            if (it == m_voices.end()) return;

            voice = it->second;
            if (voice->IsLoading)
            {
                ScheduleVoiceRemoval(voiceID);
                return;
            }

            if (OnFinishedCallback)
                OnFinishedCallback(voiceID);

            bankID = voice->BankID;
            voice->BankID = 0;

            if (voice->SourceVoice)
            {
                voice->SourceVoice->Stop();
                voice->SourceVoice->DisableEffect(CHAIN_REVERB);
                voice->SourceVoice->DisableEffect(CHAIN_EQ);
                voice->SourceVoice->DisableEffect(CHAIN_ECHO);
            }

            voice->Reset();

            // CRITICAL FIX: Don't erase yet - just set to nullptr to avoid XAudio2 callbacks during lock
            m_voices[voiceID] = nullptr;

            // Gestion du pool
            if (g_inactiveVoices.size() >= MAX_POOL_SIZE)
                g_trashBin.push_back(voice);
            else
                g_inactiveVoices.push_back(voice);

            // Check if we should remove bank (will check again under bankMutex)
            shouldCheckRemoval = (bankID > 0);
        }

        // NOW erase safely outside the lock
        {
            std::lock_guard<std::recursive_mutex> lock(m_voiceMutex);
            m_voices.erase(voiceID);
        }

        // Étape 2 : hors de tout verrou voix
        if (!shouldCheckRemoval) return;

        bool shouldRemove = false;
        {
            std::lock_guard<std::recursive_mutex> bankLock(m_bankMutex);
            auto it_bank = m_bank.find(bankID);
            if (it_bank != m_bank.end() && it_bank->second.autoRemove)
            {
                // CRITICAL FIX: Use a snapshot approach - check voice count without re-locking
                shouldRemove = true;

                {
                    std::lock_guard<std::recursive_mutex> voiceLock(m_voiceMutex);
                    // Quick check: is there any voice using this bank?
                    for (const auto& pair : m_voices)
                    {
                        if (pair.second && pair.second->BankID == bankID)
                        {
                            shouldRemove = false;
                            break;
                        }
                    }
                }
            }
        }

        if (shouldRemove)
            RemoveBankEntry(bankID);
    }

    AudioVoice* SaXAudio::GetVoice(const INT32 voiceID)
    {
        if (!m_XAudio) return nullptr;
        lock_guard<recursive_mutex> lock(m_voiceMutex);
        auto it = m_voices.find(voiceID);
        return (it != m_voices.end()) ? it->second : nullptr;
    }

    inline void GetEffectData(INT32 voiceID, BOOL isBus, IXAudio2Voice** sourceVoice, EffectData** data)
    {
        *sourceVoice = nullptr; *data = nullptr;
        if (isBus)
        {
            BusData* bus = SaXAudio::Instance.GetBus(voiceID);
            if (!bus || !bus->voice) return;
            *data = bus; *sourceVoice = bus->voice;
            return;
        }
        AudioVoice* voice = SaXAudio::Instance.GetVoice(voiceID);
        if (!voice) return;
        if (voice->BusID != 0)
        {
            BusData* bus = SaXAudio::Instance.GetBus(voice->BusID);
            if (!bus || !bus->voice) return;
            *data = bus; *sourceVoice = bus->voice;
            return;
        }
        if (!voice->SourceVoice) return;
        *data = &voice->EffectData; *sourceVoice = voice->SourceVoice;
    }

    void SaXAudio::CreateEffectChain(IXAudio2Voice* voice, EffectData* data)
    {
        if (!m_XAudio || !voice || !data) return;
        if (data->effectChain.pEffectDescriptors) return;

        HRESULT hr = S_OK;
        hr = XAudio2CreateReverb(&data->descriptors[CHAIN_REVERB].pEffect);
        if (FAILED(hr)) return;
        hr = CreateFX(__uuidof(FXEQ), &data->descriptors[CHAIN_EQ].pEffect);
        if (FAILED(hr)) return;
        FXECHO_INITDATA init = { 3000 };
        hr = CreateFX(__uuidof(FXEcho), &data->descriptors[CHAIN_ECHO].pEffect, &init, sizeof(init));
        if (FAILED(hr)) return;

        XAUDIO2_VOICE_DETAILS details {};
        voice->GetVoiceDetails(&details);
        for (int i = 0; i < 3; ++i)
        {
            data->descriptors[i].InitialState = FALSE;
            data->descriptors[i].OutputChannels = details.InputChannels;
        }
        data->effectChain.EffectCount = 3;
        data->effectChain.pEffectDescriptors = data->descriptors;

        hr = voice->SetEffectChain(&data->effectChain);
        if (FAILED(hr))
        {
            for (int i = 0; i < 3; i++) if (data->descriptors[i].pEffect) { data->descriptors[i].pEffect->Release(); data->descriptors[i].pEffect = nullptr; }
            data->effectChain.EffectCount = 0;
            data->effectChain.pEffectDescriptors = nullptr;
            return;
        }
        // Pas de Release ici, on garde la propriété des effets jusqu'à la mort de la voix
    }

    void SaXAudio::SetReverb(const INT32 voiceID, const BOOL isBus, const XAUDIO2FX_REVERB_PARAMETERS* params, const FLOAT fade)
    {
        if (!m_XAudio) return;
        IXAudio2Voice* voice = nullptr; EffectData* data = nullptr;
        GetEffectData(voiceID, isBus, &voice, &data);
        if (!voice) return;
        if (!data->effectChain.pEffectDescriptors) CreateEffectChain(voice, data);

        voice->EnableEffect(CHAIN_REVERB);
        if (data->reverbFadeID > 0) { Fader::Instance.StopFade(data->reverbFadeID); data->reverbFadeID = 0; }

        if (fade <= 0)
        {
            data->reverb = *params;
            voice->SetEffectParameters(CHAIN_REVERB, &data->reverb, sizeof(XAUDIO2FX_REVERB_PARAMETERS), XAUDIO2_COMMIT_NOW);
            return;
        }
        data->reverb.DisableLateField = params->DisableLateField;
        FLOAT* current = new FLOAT[23]; FLOAT* targets = new FLOAT[23];
        memset(current, 0, 23 * sizeof(FLOAT)); memset(targets, 0, 23 * sizeof(FLOAT));
        INT64 context = isBus ? -voiceID : voiceID;
        data->reverbFadeID = Fader::Instance.StartFadeMulti(23, current, targets, fade, OnFadeReverb, context);
    }

    void SaXAudio::RemoveReverb(const INT32 voiceID, const BOOL isBus, const FLOAT fade)
    {
        if (!m_XAudio) return;
        IXAudio2Voice* voice = nullptr; EffectData* data = nullptr;
        GetEffectData(voiceID, isBus, &voice, &data);
        if (!voice) return;
        if (data->reverbFadeID > 0) { Fader::Instance.StopFade(data->reverbFadeID); data->reverbFadeID = 0; }
        if (fade <= 0) { voice->DisableEffect(CHAIN_REVERB); return; }

        FLOAT* current = new FLOAT[23]; FLOAT* targets = new FLOAT[23];
        memset(current, 0, 23 * sizeof(FLOAT)); memset(targets, 0, 23 * sizeof(FLOAT));
        INT64 context = isBus ? -voiceID : voiceID;
        data->reverbFadeID = Fader::Instance.StartFadeMulti(23, current, targets, fade, OnFadeReverbDisable, context);
    }

    void SaXAudio::SetEq(const INT32 voiceID, const BOOL isBus, const FXEQ_PARAMETERS* params, const FLOAT fade)
    {
        if (!m_XAudio) return;
        IXAudio2Voice* voice = nullptr; EffectData* data = nullptr;
        GetEffectData(voiceID, isBus, &voice, &data);
        if (!voice) return;
        if (!data->effectChain.pEffectDescriptors) CreateEffectChain(voice, data);
        voice->EnableEffect(CHAIN_EQ);
        if (data->eqFadeID > 0) { Fader::Instance.StopFade(data->eqFadeID); data->eqFadeID = 0; }
        if (fade <= 0)
        {
            data->eq = *params;
            voice->SetEffectParameters(CHAIN_EQ, &data->eq, sizeof(FXEQ_PARAMETERS), XAUDIO2_COMMIT_NOW);
            return;
        }
        // Fader...
    }
    void SaXAudio::RemoveEq(const INT32 voiceID, const BOOL isBus, const FLOAT fade)
    {
        if (!m_XAudio) return;
        IXAudio2Voice* voice = nullptr; EffectData* data = nullptr;
        GetEffectData(voiceID, isBus, &voice, &data);
        if (!voice) return;
        if (data->eqFadeID > 0) { Fader::Instance.StopFade(data->eqFadeID); data->eqFadeID = 0; }
        if (fade <= 0) { voice->DisableEffect(CHAIN_EQ); return; }
        // Fader...
    }
    void SaXAudio::SetEcho(const INT32 voiceID, const BOOL isBus, const FXECHO_PARAMETERS* params, const FLOAT fade)
    {
        if (!m_XAudio) return;
        IXAudio2Voice* voice = nullptr; EffectData* data = nullptr;
        GetEffectData(voiceID, isBus, &voice, &data);
        if (!voice) return;
        if (!data->effectChain.pEffectDescriptors) CreateEffectChain(voice, data);
        voice->EnableEffect(CHAIN_ECHO);
        if (data->echoFadeID > 0) { Fader::Instance.StopFade(data->echoFadeID); data->echoFadeID = 0; }
        if (fade <= 0)
        {
            data->echo = *params;
            voice->SetEffectParameters(CHAIN_ECHO, &data->echo, sizeof(FXECHO_PARAMETERS), XAUDIO2_COMMIT_NOW);
            return;
        }
        // Fader...
    }
    void SaXAudio::RemoveEcho(const INT32 voiceID, const BOOL isBus, const FLOAT fade)
    {
        if (!m_XAudio) return;
        IXAudio2Voice* voice = nullptr; EffectData* data = nullptr;
        GetEffectData(voiceID, isBus, &voice, &data);
        if (!voice) return;
        if (data->echoFadeID > 0) { Fader::Instance.StopFade(data->echoFadeID); data->echoFadeID = 0; }
        if (fade <= 0) { voice->DisableEffect(CHAIN_ECHO); return; }
        // Fader...
    }

    void SaXAudio::OnFadeReverb(INT64 context, UINT32 count, FLOAT* newValues, BOOL hasFinished) { /*...*/ if (hasFinished) { /*reset ID*/ } }
    void SaXAudio::OnFadeReverbDisable(INT64 context, UINT32 count, FLOAT* newValues, BOOL hasFinished) { /*...*/ }
    void SaXAudio::OnFadeEq(INT64 context, UINT32 count, FLOAT* newValues, BOOL hasFinished) { /*...*/ }
    void SaXAudio::OnFadeEqDisable(INT64 context, UINT32 count, FLOAT* newValues, BOOL hasFinished) { /*...*/ }
    void SaXAudio::OnFadeEcho(INT64 context, UINT32 count, FLOAT* newValues, BOOL hasFinished) { /*...*/ }
    void SaXAudio::OnFadeEchoDisable(INT64 context, UINT32 count, FLOAT* newValues, BOOL hasFinished) { /*...*/ }

    void SaXAudio::DecodeOgg(const INT32 bankID, stb_vorbis* vorbis)
    {
        stb_vorbis_seek_start(vorbis);

        UINT32 samplesTotal = 0;
        UINT32 samplesDecoded = 0;

        // Optimisation : Buffer plus grand pour réduire la fréquence des locks
        UINT32 bufferSize = 16384;

        // 1. Initialisation thread-safe (on ne touche pas à m_bank sans lock)
        // Mais on a déjà le pointeur 'vorbis', donc on peut interroger stb_vorbis directement
        stb_vorbis_info info = stb_vorbis_get_info(vorbis);
        samplesTotal = stb_vorbis_stream_length_in_samples(vorbis);
        UINT32 channels = info.channels;

        // Buffer temporaire sur la pile (ou heap si trop gros) pour le décodage parallèle
        // Attention à la taille de la pile, 16384 * 2 * 4 bytes ~= 128KB, c'est acceptable sur Windows
        std::vector<float> tempBuffer(bufferSize * channels);

        while (samplesTotal - samplesDecoded > 0)
        {
            UINT32 currentRequest = bufferSize;
            if (currentRequest > samplesTotal - samplesDecoded)
                currentRequest = samplesTotal - samplesDecoded;

            // --- PARTIE LOURDE (SANS LOCK) ---
            // Le décodage se fait ici, en parallèle des autres threads
            UINT32 decoded = stb_vorbis_get_samples_float_interleaved(vorbis, channels, tempBuffer.data(), currentRequest * channels);

            if (decoded == 0) break; // Fin du stream ou erreur

            // --- PARTIE CRITIQUE (AVEC LOCK) ---
            // On prend le lock uniquement pour copier le résultat en RAM
            {
                lock_guard<recursive_mutex> bankLock(SaXAudio::Instance.m_bankMutex);

                // On vérifie que la banque existe encore (au cas où RemoveBank a été appelé)
                auto it = SaXAudio::Instance.m_bank.find(bankID);
                if (it == SaXAudio::Instance.m_bank.end())
                    break; // La banque a été supprimée, on arrête tout

                BankData* data = &it->second;
                if (!data || data->disposed)
                    break;

                {
                    lock_guard<mutex> lock(data->decodingMutex);

                    // Copie rapide (Memory Bandwidth bound, très rapide)
                    FLOAT* pDest = &data->buffer.Data[samplesDecoded * channels];
                    memcpy(pDest, tempBuffer.data(), decoded * channels * sizeof(float));

                    samplesDecoded += decoded;
                    data->decodedSamples = samplesDecoded;

                    // Si on a fini, on met à jour le total exact
                    if (samplesDecoded >= samplesTotal)
                        data->totalSamples = samplesDecoded;

                    data->decodingPerform.notify_all();
                }
            }
        }

        stb_vorbis_close(vorbis);

        // Callback final
        {
            lock_guard<recursive_mutex> bankLock(SaXAudio::Instance.m_bankMutex);
            auto it = SaXAudio::Instance.m_bank.find(bankID);
            if (it != SaXAudio::Instance.m_bank.end())
            {
                BankData* data = &it->second;
                if (data && data->onDecodedCallback)
                {
                    (*data->onDecodedCallback)(bankID, data->Oggbuffer);
                    data->onDecodedCallback = nullptr;
                }
            }
        }
    }

    UINT32 SaXAudio::GetVoiceCount(const INT32 bankID, const INT32 busID) { return 0; }
    UINT32 SaXAudio::GetBankCount() { return 0; }

    void EffectData::Reset()
    {
        reverb = {}; eq = {}; echo = {};
        if (reverbFadeID > 0) { Fader::Instance.StopFade(reverbFadeID); reverbFadeID = 0; }
        if (eqFadeID > 0) { Fader::Instance.StopFade(eqFadeID); eqFadeID = 0; }
        if (echoFadeID > 0) { Fader::Instance.StopFade(echoFadeID); echoFadeID = 0; }
    }

    void SaXAudio::ScheduleVoiceRemoval(INT32 voiceID)
    {
        std::lock_guard<std::recursive_mutex> lock(m_gcMutex);
        if (std::find(m_garbageCollectionQueue.begin(), m_garbageCollectionQueue.end(), voiceID) == m_garbageCollectionQueue.end())
        {
            m_garbageCollectionQueue.push_back(voiceID);
        }
    }

    static int debugFrameCount = 0;

    void SaXAudio::Update()
    {
        debugFrameCount++;
        if (debugFrameCount > 300)
        {
            debugFrameCount = 0;

            // 1. Nettoyage RAM (Buffers) - Protégé par BankMutex
            {
                std::lock_guard<std::recursive_mutex> bLock(m_bankMutex);
                while (m_bufferPool.size() > MAX_UNUSED_BUFFERS)
                {
                    Buffer buf = m_bufferPool.front();
                    delete[] buf.Data;
                    m_bufferPool.pop_front();
                }
            }

            // 2. VIDAGE DU PURGATOIRE (La correction du FREEZE)
            vector<AudioVoice*> toDestroy;
            {
                std::lock_guard<std::recursive_mutex> vLock(m_voiceMutex);

                size_t limit = IsDebuggerPresent() ? 2 : g_trashBin.size();
                size_t count = 0;
                while (count < limit && !g_trashBin.empty())
                {
                    toDestroy.push_back(g_trashBin.back());
                    g_trashBin.pop_back();
                    count++;
                }
            }

            // DESTRUCTION HORS LOCK (Safe Deadlock)
            for (AudioVoice* v : toDestroy)
            {
                DestroyVoicePhysical(v);
            }

            // 3. Stats pour le log
            size_t voicesCount, inactiveCount, banksCount, buffersCount;
            {
                std::lock_guard<std::recursive_mutex> vLock(m_voiceMutex);
                voicesCount = m_voices.size();
                inactiveCount = g_inactiveVoices.size();
            }
            {
                std::lock_guard<std::recursive_mutex> bLock(m_bankMutex);
                banksCount = m_bank.size();
                buffersCount = m_bufferPool.size();
            }

            PROCESS_MEMORY_COUNTERS pmc;
            SIZE_T ramUsage = 0;
            if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
            {
                ramUsage = pmc.WorkingSetSize / (1024 * 1024);
            }

            string stats = "[STATS] Voices: " + to_string(voicesCount) +
                " | Pool: " + to_string(inactiveCount) +
                " | Banks: " + to_string(banksCount) +
                " | Buffers: " + to_string(buffersCount) +
                " | RAM: " + to_string(ramUsage) + " MB\n";
            OutputDebugStringA(stats.c_str());

            return;  // <-- CRITICAL: Exit early to avoid garbage collection processing
        }

        // Process garbage collection ONLY outside the 600-frame periodic cleanup
        std::vector<INT32> processQueue;
        {
            std::lock_guard<std::recursive_mutex> lock(m_gcMutex);
            if (m_garbageCollectionQueue.empty()) return;
            processQueue.swap(m_garbageCollectionQueue);
        }

        for (INT32 id : processQueue)
        {
            RemoveVoice(id);
        }
    }
}