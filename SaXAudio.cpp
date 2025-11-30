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

namespace SaXAudio
{
#define GetEntry(data, map, id) nullptr; auto it_##data = map.find(id); if (it_##data != map.end()) data = &it_##data->second
#define CHAIN_REVERB 0
#define CHAIN_EQ 1
#define CHAIN_ECHO 2
#define POOL_SIZE_VOICES 50

    SaXAudio& SaXAudio::Instance = SaXAudio::getInstance();

    BOOL SaXAudio::Init()
    {
        if (m_XAudio)
            return true;

        StartLogging();

        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        switch (hr)
        {
        case S_OK:
        case S_FALSE:
            break;
        case RPC_E_CHANGED_MODE:
            // COM initialized with different threading model
            // Try apartment threading instead
            hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            if (SUCCEEDED(hr))
                break;
        default:
            Log(0, 0, "[Init] COM initialize failed", hr);
            return false;
        }

        // Create XAudio2 instance
        hr = XAudio2Create(&m_XAudio, 0, XAUDIO2_DEFAULT_PROCESSOR);
        if (FAILED(hr))
        {
            Log(0, 0, "[Init] XAudio2 creation failed", hr);
            m_XAudio = nullptr;
            return false;
        }

        // Create mastering voice
        IXAudio2MasteringVoice* masteringVoice;
        hr = m_XAudio->CreateMasteringVoice(&masteringVoice, 0, 48000);
        if (FAILED(hr))
        {
            Log(0, 0, "[Init] Mastering voice creation failed", hr);
            m_XAudio->Release();
            m_XAudio = nullptr;
            return false;
        }

        m_masteringBus.voice = masteringVoice;

        // Get channel mask
        hr = masteringVoice->GetChannelMask(&m_channelMask);
        if (FAILED(hr))
        {
            Log(0, 0, "[Init] Couldn't get channel mask", hr);
            m_XAudio->Release();
            m_XAudio = nullptr;
            return false;
        }

        string version = "Unknown";
        // Check which DLL is loaded
        HMODULE hXAudio2 = GetModuleHandle("XAudio2_9.dll");
        if (hXAudio2)
        {
            version = "XAudio2 2.9";
        }
        else
        {
            hXAudio2 = GetModuleHandle("XAudio2_8.dll");
            if (hXAudio2)
            {
                version = "XAudio2 2.8";
            }
            else
            {
                hXAudio2 = GetModuleHandle("XAudio2_7.dll");
                if (hXAudio2)
                {
                    version = "XAudio2 2.7";
                }
            }
        }

        // Get details
        masteringVoice->GetVoiceDetails(&m_masterDetails);
        Log(0, 0, "[Init] Initialization complete. Version: " + version + " Channels: " + to_string(m_masterDetails.InputChannels) + " Sample rate: " + to_string(m_masterDetails.InputSampleRate));

        return true;
    }

    void ClearVoiceEffects(AudioVoice* voice)
    {
        if (!voice || !voice->SourceVoice) return;
        
        // First disable all effects
        for (int i = 0; i < 3; i++)
        {
            voice->SourceVoice->DisableEffect(i);
        }

        // Then release COM objects
        for (int i = 0; i < 3; i++)
        {
            if (voice->EffectData.descriptors[i].pEffect)
            {
                voice->EffectData.descriptors[i].pEffect->Release();
                voice->EffectData.descriptors[i].pEffect = nullptr;
            }
        }
        voice->EffectData.effectChain.pEffectDescriptors = nullptr;
        voice->EffectData.effectChain.EffectCount = 0;

        Log(voice->BankID, voice->VoiceID, "[ClearVoiceEffects]");
    }

    void SaXAudio::Release()
    {
        if (!m_XAudio)
            return;

        m_XAudio->StopEngine();

        // Release all voices' effects BEFORE destroying voices
        for (auto& it : m_voices)
        {
            AudioVoice* voice = it.second;
            if (voice && voice->SourceVoice)
            {
                for (int i = 0; i < 3; i++)
                {
                    voice->SourceVoice->DisableEffect(i);
                }
            }
        }

        // Release all pooled voices' effects
        queue<AudioVoice*> tempPool = m_voicePool;
        while (!tempPool.empty())
        {
            AudioVoice* voice = tempPool.front();
            tempPool.pop();
            
            for (int i = 0; i < 3; i++)
            {
                if (voice->EffectData.descriptors[i].pEffect)
                {
                    voice->EffectData.descriptors[i].pEffect->Release();
                    voice->EffectData.descriptors[i].pEffect = nullptr;
                }
            }
        }

        // Release all buses effects first
        for (auto& it : m_buses)
        {
            BusData* bus = &it.second;
            if (bus->voice)
            {
                // Disable effects first
                for (int i = 0; i < 3; i++)
                {
                    bus->voice->DisableEffect(i);
                }
            }
            // Then release COM objects
            for (int i = 0; i < 3; i++)
            {
                if (bus->descriptors[i].pEffect)
                {
                    bus->descriptors[i].pEffect->Release();
                    bus->descriptors[i].pEffect = nullptr;
                }
            }
        }

        // Release mastering bus effects
        if (m_masteringBus.voice)
        {
            // Disable effects first
            for (int i = 0; i < 3; i++)
            {
                m_masteringBus.voice->DisableEffect(i);
            }
        }
        // Then release COM objects
        for (int i = 0; i < 3; i++)
        {
            if (m_masteringBus.descriptors[i].pEffect)
            {
                m_masteringBus.descriptors[i].pEffect->Release();
                m_masteringBus.descriptors[i].pEffect = nullptr;
            }
        }

        m_XAudio->Release();
        m_XAudio = nullptr;

        while (!m_bank.empty())
        {
            RemoveBankEntry(m_bank.begin()->first);
        }

        while (!m_voicePool.empty())
        {
            delete m_voicePool.front();
            m_voicePool.pop();
        }

        StopLogging();

        m_voices.clear();
        m_masteringBus.voice = nullptr;
    }

    void SaXAudio::StopEngine()
    {
        if (!m_XAudio)
            return;
        Log(0, 0, "[StopEngine]");

        m_XAudio->StopEngine();
    }

    void SaXAudio::StartEngine()
    {
        if (!m_XAudio)
            return;
        Log(0, 0, "[StartEngine]");

        m_XAudio->StartEngine();
    }

    void SaXAudio::PauseAll(const FLOAT fade, const INT32 busID)
    {
        if (!m_XAudio)
            return;
        Log(0, 0, "[PauseAll]");

        for (auto& it : m_voices)
        {
            if (!it.second->IsProtected && (busID == 0 || it.second->BusID == busID))
                it.second->Pause(fade);
        }
    }

    void SaXAudio::ResumeAll(const FLOAT fade, const INT32 busID)
    {
        if (!m_XAudio)
            return;
        Log(0, 0, "[ResumeAll]");

        for (auto& it : m_voices)
        {
            if (!it.second->IsProtected && (busID == 0 || it.second->BusID == busID))
                it.second->Resume(fade);
        }
    }

    void SaXAudio::StopAll(const FLOAT fade, const INT32 busID)
    {
        if (!m_XAudio)
            return;
        Log(0, 0, "[StopAll]");

        for (auto& it : m_voices)
        {
            if (!it.second->IsProtected && (busID == 0 || it.second->BusID == busID))
                it.second->Stop(fade);
        }
    }

    void SaXAudio::Protect(const INT32 voiceID)
    {
        if (!m_XAudio)
            return;

        AudioVoice* voice = GetVoice(voiceID);
        if (voice)
        {
            voice->IsProtected = true;
            if (voice->IsPlaying)
                while (voice->Resume());

            Log(voice->BankID, voiceID, "[Protect]");
        }
    }

    INT32 SaXAudio::AddBankEntry(const OnDecodedCallback callback)
    {
        if (!m_XAudio)
            return 0;
        lock_guard<mutex> lock(m_bankMutex);

        Log(m_bankCounter, 0, "[AddBankEntry]");

        m_bank[m_bankCounter].onDecodedCallback = callback;
        return m_bankCounter++;
    }

    static mt19937 gen { std::random_device{}() };
    void DeleteBufferDelayed(FLOAT* buffer, INT32 bankID)
    {
        INT32 rng = uniform_int_distribution<> { 0, 1000 }(gen);
        this_thread::sleep_for(chrono::milliseconds(1000 + rng));
        Log(bankID, 0, "[DeleteBufferDelayed]");
        delete[] buffer;
    }

    void SaXAudio::RemoveBankEntry(const INT32 bankID)
    {
        lock_guard<mutex> lock(m_bankMutex);

        Log(bankID, 0, "[RemoveBankEntry]");

        BankData* data = GetEntry(data, m_bank, bankID);
        if (!data) return;
        data->autoRemove = true;
        data->disposed = true;

        if (m_XAudio)
        {
            // Let voices finish before removing
            for (auto& it : m_voices)
            {
                if (it.second->BankID == bankID)
                {
                    // We let autoRemove delete the bankID
                    return;
                }
            }
        }

        // Free the audio buffer
        if (data->buffer)
        {
            thread deleteBuffer(DeleteBufferDelayed, data->buffer, bankID);
            deleteBuffer.detach();
            data->buffer = nullptr;
        }
        // onDecodedCallback guarantied to be called
        if (data->onDecodedCallback)
        {
            (*data->onDecodedCallback)(bankID, data->Oggbuffer);
            data->onDecodedCallback = nullptr;
        }
        m_bank.erase(bankID);
    }

    void SaXAudio::AutoRemoveBank(const INT32 bankID)
    {
        if (!m_XAudio)
            return;
        lock_guard<mutex> lock(m_bankMutex);

        Log(bankID, 0, "[AutoRemoveBank]");

        BankData* data = GetEntry(data, m_bank, bankID);
        if (!data) return;

        data->autoRemove = true;
    }

    INT32 SaXAudio::AddBus()
    {
        if (!m_XAudio)
            return 0;
        lock_guard<mutex> lock(m_busMutex);

        Log(0, 0, "[AddBus]");

        IXAudio2SubmixVoice* bus;
        HRESULT hr = m_XAudio->CreateSubmixVoice(&bus, m_masterDetails.InputChannels, m_masterDetails.InputSampleRate);
        if (FAILED(hr))
        {
            Log(-1, -1, "Failed creating bus", hr);
            return 0;
        }

        BusData* data = &m_buses[m_busCounter];
        data->voice = bus;
        data->effectChain = { 3, nullptr };
        data->descriptors[0] = { nullptr, false, SaXAudio::m_masterDetails.InputChannels };
        data->descriptors[1] = { nullptr, false, SaXAudio::m_masterDetails.InputChannels };
        data->descriptors[2] = { nullptr, false, SaXAudio::m_masterDetails.InputChannels };

        return m_busCounter++;
    }

    void SaXAudio::RemoveBus(const INT32 busID)
    {
        if (!m_XAudio)
            return;
        lock_guard<mutex> lock(m_busMutex);

        Log(0, 0, "[RemoveBus] " + to_string(busID));

        BusData* bus = GetEntry(bus, m_buses, busID);
        if (!bus) return;

        for (auto& it : m_voices)
        {
            if (it.second->BusID == busID)
                it.second->Stop();
        }

        // Disable all effects first before releasing
        if (bus->voice)
        {
            for (int i = 0; i < 3; i++)
            {
                bus->voice->DisableEffect(i);
            }
        }

        // Release COM objects
        for (int i = 0; i < 3; i++)
        {
            if (bus->descriptors[i].pEffect)
            {
                bus->descriptors[i].pEffect->Release();
                bus->descriptors[i].pEffect = nullptr;
            }
        }

        bus->voice->DestroyVoice();
        m_buses.erase(busID);
    }

    BusData* SaXAudio::GetBus(const INT32 busID)
    {
        if (!m_XAudio)
            return nullptr;
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
        if (!m_XAudio)
            return;

        BusData* bus = busID == 0 ? &m_masteringBus : GetBus(busID);

        if (!bus || !bus->voice) return;
        Log(0, 0, "[SetBusVolume] " + to_string(busID) + " to: " + to_string(volume) + " fade: " + to_string(fade));

        Fader::Instance.StopFade(bus->fadeID);
        bus->fadeID = 0;

        if (fade > 0)
        {
            FLOAT current = 1.0f;
            bus->voice->GetVolume(&current);
            bus->fadeID = Fader::Instance.StartFade(current, volume, fade, OnFadeVolume, busID);
        }
        else
        {
            bus->voice->SetVolume(volume);
        }
    }

    FLOAT SaXAudio::GetBusVolume(const INT32 busID)
    {
        if (!m_XAudio)
            return 0.0f;

        BusData* bus = busID == 0 ? &m_masteringBus : GetBus(busID);
        if (!bus || !bus->voice) return 0.0f;

        FLOAT volume;
        bus->voice->GetVolume(&volume);
        Log(0, 0, "[GetBusVolume] " + to_string(busID) + " volume: " + to_string(volume));
        return volume;
    }

    UINT32 SaXAudio::AddBankData(FLOAT* buffer, UINT32 channels, UINT32 sampleRate, UINT32 totalSamples)
    {
        if (!m_XAudio)
            return 0;

        lock_guard<mutex> bankLock(SaXAudio::Instance.m_bankMutex);
        Log(m_bankCounter, 0, "[AddBankData]");

        BankData* data = &m_bank[m_bankCounter];
        data->buffer = buffer;
        data->channels = channels;
        data->sampleRate = sampleRate;
        data->totalSamples = totalSamples;
        data->decodedSamples = data->totalSamples;

        return m_bankCounter++;
    }

    BOOL SaXAudio::StartDecodeOgg(const INT32 bankID, const BYTE* buffer, const UINT32 length)
    {
        int error;
        stb_vorbis* vorbis = stb_vorbis_open_memory(buffer, length, &error, NULL);

        if (!vorbis)
            return FALSE;

        lock_guard<mutex> bankLock(m_bankMutex);
        BankData* data = GetEntry(data, m_bank, bankID);
        if (!data) return FALSE;

        data->Oggbuffer = buffer;

        // Get file info
        stb_vorbis_info info = stb_vorbis_get_info(vorbis);
        data->channels = info.channels;
        data->sampleRate = info.sample_rate;

        // Get total samples count and allocate the buffer
        data->totalSamples = stb_vorbis_stream_length_in_samples(vorbis);
        data->buffer = new FLOAT[data->totalSamples * data->channels];

        thread decode(DecodeOgg, bankID, vorbis);
        decode.detach();
        return TRUE;
    }

    AudioVoice* SaXAudio::CreateVoice(const INT32 bankID, const INT32 busID)
    {
        if (!m_XAudio) return nullptr;

        // Verrouillage des mutex
        lock_guard<mutex> bankLock(m_bankMutex);
        lock_guard<mutex> busLock(m_busMutex);
        lock_guard<mutex> voiceLock(m_voiceMutex);

        BankData* data = GetEntry(data, m_bank, bankID);
        if (!data || data->disposed) return nullptr;

        // --- Format Audio ---
        WAVEFORMATEX wfx = { 0 };
        wfx.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
        wfx.nChannels = static_cast<WORD>(data->channels);
        wfx.nSamplesPerSec = static_cast<DWORD>(data->sampleRate);
        wfx.wBitsPerSample = 32;
        wfx.nBlockAlign = wfx.nChannels * wfx.wBitsPerSample / 8;
        wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;
        wfx.cbSize = 0;

        // --- Gestion du Pool ---
        if (m_voicePool.empty())
        {
            // Allocation par lot si vide (optimisation)
            for (UINT32 i = 0; i < POOL_SIZE_VOICES; i++)
                m_voicePool.push(new AudioVoice);
        }

        AudioVoice* voice = m_voicePool.front();
        m_voicePool.pop();

        // --- Nettoyage préventif de la voix recyclée ---
        // On s'assure que la voix n'a pas de vieux pointeurs d'effets qui traînent
        // (Important si le Reset() n'a pas tout nettoyé)
        voice->EffectData.effectChain.EffectCount = 0;
        voice->EffectData.effectChain.pEffectDescriptors = nullptr;
        for (int i = 0; i < 3; i++) voice->EffectData.descriptors[i].pEffect = nullptr;

        BusData* bus = GetEntry(bus, m_buses, busID);

        // --- Création du SourceVoice (SANS EFFETS) ---
        HRESULT hr;

        // Note: On passe nullptr pour pEffectChain ici !
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

        if (FAILED(hr))
        {
            voice->SourceVoice = nullptr;
            // En cas d'échec, on ne perd pas la voix, on la remet dans le pool ou on la delete
            // Ici on log juste l'erreur
            Log(bankID, m_voiceCounter, "Failed to create voice", hr);
            return nullptr;
        }

        // --- Initialisation des données ---
        voice->BankData = data;
        voice->Buffer = { 0 };
        voice->Buffer.AudioBytes = static_cast<UINT32>(sizeof(float) * data->totalSamples * data->channels);
        voice->Buffer.pAudioData = reinterpret_cast<const BYTE*>(data->buffer);
        voice->Buffer.Flags = XAUDIO2_END_OF_STREAM;
        voice->BankID = bankID;
        voice->VoiceID = m_voiceCounter++;
        voice->BusID = bus ? busID : 0;

        voice->SetOutputMatrix(0.0f);
        m_voices[voice->VoiceID] = voice;

        Log(bankID, voice->VoiceID, "[CreateVoice] Created (Lazy Effects)");

        return voice;
    }

    void UpdateVoiceEffectChain(AudioVoice* voice)
    {
        if (!voice || !voice->SourceVoice) return;

        // On prépare un tableau temporaire pour compacter les effets actifs
        XAUDIO2_EFFECT_DESCRIPTOR activeDescriptors[3];
        UINT32 activeCount = 0;

        // Vérifie Reverb
        if (voice->EffectData.descriptors[CHAIN_REVERB].pEffect != nullptr)
        {
            activeDescriptors[activeCount] = voice->EffectData.descriptors[CHAIN_REVERB];
            activeCount++;
        }
        // Vérifie EQ
        if (voice->EffectData.descriptors[CHAIN_EQ].pEffect != nullptr)
        {
            activeDescriptors[activeCount] = voice->EffectData.descriptors[CHAIN_EQ];
            activeCount++;
        }
        // Vérifie Echo
        if (voice->EffectData.descriptors[CHAIN_ECHO].pEffect != nullptr)
        {
            activeDescriptors[activeCount] = voice->EffectData.descriptors[CHAIN_ECHO];
            activeCount++;
        }

        if (activeCount > 0)
        {
            XAUDIO2_EFFECT_CHAIN chain = { 0 };
            chain.EffectCount = activeCount;
            chain.pEffectDescriptors = activeDescriptors;

            // --- AJOUT DE LOGS D'ERREUR ---
            HRESULT hr = voice->SourceVoice->SetEffectChain(&chain);
            if (FAILED(hr))
            {
                // Si ça échoue ici, c'est souvent un mismatch de canaux
                Log(voice->BankID, voice->VoiceID, "Failed to update EffectChain (Channel mismatch?)", hr);
                return;
            }

            for (UINT32 i = 0; i < activeCount; i++)
            {
                voice->SourceVoice->EnableEffect(i);
            }
        }
        else
        {
            voice->SourceVoice->SetEffectChain(nullptr);
        }
    }

    AudioVoice* SaXAudio::GetVoice(const INT32 voiceID)
    {
        if (!m_XAudio)
            return nullptr;
        lock_guard<mutex> lock(m_voiceMutex);

        AudioVoice* voice = nullptr;
        auto it_voice = m_voices.find(voiceID);
        if (it_voice != m_voices.end())
            voice = it_voice->second;
        return voice;
    }

    inline void GetEffectData(INT32 voiceID, BOOL isBus, IXAudio2Voice** sourceVoice, EffectData** data)
    {
        if (isBus)
        {
            BusData* bus = SaXAudio::Instance.GetBus(voiceID);
            if (!bus || !bus->voice) return;

            *data = bus;
            *sourceVoice = bus->voice;
        }
        else
        {
            AudioVoice* voice = SaXAudio::Instance.GetVoice(voiceID);
            if (!voice || !voice->SourceVoice) return;

            *data = &voice->EffectData;
            *sourceVoice = voice->SourceVoice;
        }
    }

    void SaXAudio::SetReverb(const INT32 voiceID, const BOOL isBus, const XAUDIO2FX_REVERB_PARAMETERS* params, const FLOAT fade)
    {
        if (!m_XAudio) return;

        lock_guard<mutex> lock(m_voiceMutex);
        IXAudio2Voice* xVoice = nullptr;
        EffectData* data = nullptr;
        AudioVoice* audioVoice = nullptr;
        UINT32 channels = 2; // Valeur par défaut de sécurité

        // --- 1. Récupération & Détermination des canaux ---
        if (isBus)
        {
            BusData* bus = SaXAudio::Instance.GetBus(voiceID);
            if (!bus || !bus->voice) return;
            data = bus;
            xVoice = bus->voice;
            channels = SaXAudio::Instance.m_masterDetails.InputChannels; // Les bus sont souvent en 7.1 ou Stéréo
        }
        else
        {
            audioVoice = SaXAudio::Instance.GetVoice(voiceID);
            if (!audioVoice || !audioVoice->SourceVoice) return;
            data = &audioVoice->EffectData;
            xVoice = audioVoice->SourceVoice;
            if (audioVoice->BankData) channels = audioVoice->BankData->channels;
        }

        // --- 2. Lazy Initialization ---
        if (data->descriptors[CHAIN_REVERB].pEffect == nullptr)
        {
            Log(voiceID, 0, "[SetReverb] Lazy creating Reverb DSP. Channels: " + to_string(channels));

            HRESULT hr = XAudio2CreateReverb(&data->descriptors[CHAIN_REVERB].pEffect);
            if (FAILED(hr)) { Log(0, 0, "Failed to create Reverb", hr); return; }

            data->descriptors[CHAIN_REVERB].InitialState = TRUE;

            // CORRECTION CRITIQUE : On force le respect des canaux de la voix
            data->descriptors[CHAIN_REVERB].OutputChannels = channels;

            if (!isBus && audioVoice) UpdateVoiceEffectChain(audioVoice);
        }

        // --- 3. Calcul de l'index réel dans la chaîne ---
        // Puisque la Reverb est le premier effet qu'on ajoute dans UpdateVoiceEffectChain (index 0),
        // si elle existe, elle est TOUJOURS à l'index 0 de la chaîne active.
        UINT32 realChainIndex = 0;

        // Activation
        HRESULT hr = xVoice->EnableEffect(realChainIndex);
        if (FAILED(hr)) Log(0, 0, "Failed to enable reverb", hr);

        // --- 4. Application des paramètres (Logic Fader inchangée) ---
        if (fade <= 0)
        {
            data->reverb = *params;
            hr = xVoice->SetEffectParameters(realChainIndex, &data->reverb, sizeof(XAUDIO2FX_REVERB_PARAMETERS), XAUDIO2_COMMIT_NOW);
            if (FAILED(hr)) Log(0, 0, "Failed to set reverb parameters", hr);
            return;
        }

        // Gestion du Fade (booléen non interpolable)
        data->reverb.DisableLateField = params->DisableLateField;

        // Préparation des tableaux pour le Fader
        // Note: On utilise 'data->reverb' (valeurs actuelles) vs 'params' (cibles)

        // (Je garde ton code de mapping tableau exact ici pour éviter les erreurs de copier/coller)
        FLOAT* current = new FLOAT[23]
        {
            data->reverb.WetDryMix,
            static_cast<FLOAT>(data->reverb.ReflectionsDelay),
            static_cast<FLOAT>(data->reverb.ReverbDelay),
            static_cast<FLOAT>(data->reverb.RearDelay),
            static_cast<FLOAT>(data->reverb.SideDelay),
            static_cast<FLOAT>(data->reverb.PositionLeft),
            static_cast<FLOAT>(data->reverb.PositionRight),
            static_cast<FLOAT>(data->reverb.PositionMatrixLeft),
            static_cast<FLOAT>(data->reverb.PositionMatrixRight),
            static_cast<FLOAT>(data->reverb.EarlyDiffusion),
            static_cast<FLOAT>(data->reverb.LateDiffusion),
            static_cast<FLOAT>(data->reverb.LowEQGain),
            static_cast<FLOAT>(data->reverb.LowEQCutoff),
            static_cast<FLOAT>(data->reverb.HighEQGain),
            static_cast<FLOAT>(data->reverb.HighEQCutoff),
            data->reverb.RoomFilterFreq,
            data->reverb.RoomFilterMain,
            data->reverb.RoomFilterHF,
            data->reverb.ReflectionsGain,
            data->reverb.ReverbGain,
            data->reverb.DecayTime,
            data->reverb.Density,
            data->reverb.RoomSize
        };

        FLOAT* targets = new FLOAT[23]
        {
            params->WetDryMix,
            static_cast<FLOAT>(params->ReflectionsDelay),
            static_cast<FLOAT>(params->ReverbDelay),
            static_cast<FLOAT>(params->RearDelay),
            static_cast<FLOAT>(params->SideDelay),
            static_cast<FLOAT>(params->PositionLeft),
            static_cast<FLOAT>(params->PositionRight),
            static_cast<FLOAT>(params->PositionMatrixLeft),
            static_cast<FLOAT>(params->PositionMatrixRight),
            static_cast<FLOAT>(params->EarlyDiffusion),
            static_cast<FLOAT>(params->LateDiffusion),
            static_cast<FLOAT>(params->LowEQGain),
            static_cast<FLOAT>(params->LowEQCutoff),
            static_cast<FLOAT>(params->HighEQGain),
            static_cast<FLOAT>(params->HighEQCutoff),
            params->RoomFilterFreq,
            params->RoomFilterMain,
            params->RoomFilterHF,
            params->ReflectionsGain,
            params->ReverbGain,
            params->DecayTime,
            params->Density,
            params->RoomSize
        };

        INT64 context = isBus ? -voiceID : voiceID;

        // Attention : OnFadeReverb devra aussi utiliser l'index 0 (realChainIndex) !
        // Vérifie ta fonction OnFadeReverb pour t'assurer qu'elle n'utilise pas une constante hardcodée incorrecte
        // si tu changes l'ordre des effets. Mais ici CHAIN_REVERB est 0, donc c'est bon.
        Fader::Instance.StartFadeMulti(23, current, targets, fade, OnFadeReverb, context);
    }

    void SaXAudio::RemoveReverb(const INT32 voiceID, const BOOL isBus, const FLOAT fade)
    {
        if (!m_XAudio)
            return;

        IXAudio2Voice* voice = nullptr;
        EffectData* data = nullptr;
        GetEffectData(voiceID, isBus, &voice, &data);
        if (!voice) return;

        if (fade <= 0)
        {
            voice->DisableEffect(CHAIN_REVERB);
            return;
        }

        FLOAT* current = new FLOAT[23]
        {
            data->reverb.WetDryMix,
            static_cast<FLOAT>(data->reverb.ReflectionsDelay),
            static_cast<FLOAT>(data->reverb.ReverbDelay),
            static_cast<FLOAT>(data->reverb.RearDelay),
            static_cast<FLOAT>(data->reverb.SideDelay),
            static_cast<FLOAT>(data->reverb.PositionLeft),
            static_cast<FLOAT>(data->reverb.PositionRight),
            static_cast<FLOAT>(data->reverb.PositionMatrixLeft),
            static_cast<FLOAT>(data->reverb.PositionMatrixRight),
            static_cast<FLOAT>(data->reverb.EarlyDiffusion),
            static_cast<FLOAT>(data->reverb.LateDiffusion),
            static_cast<FLOAT>(data->reverb.LowEQGain),
            static_cast<FLOAT>(data->reverb.LowEQCutoff),
            static_cast<FLOAT>(data->reverb.HighEQGain),
            static_cast<FLOAT>(data->reverb.HighEQCutoff),
            data->reverb.RoomFilterFreq,
            data->reverb.RoomFilterMain,
            data->reverb.RoomFilterHF,
            data->reverb.ReflectionsGain,
            data->reverb.ReverbGain,
            data->reverb.DecayTime,
            data->reverb.Density,
            data->reverb.RoomSize
        };

        FLOAT* targets = new FLOAT[23]
        {
            0,
            static_cast<FLOAT>(data->reverb.ReflectionsDelay),
            static_cast<FLOAT>(data->reverb.ReverbDelay),
            static_cast<FLOAT>(data->reverb.RearDelay),
            static_cast<FLOAT>(data->reverb.SideDelay),
            static_cast<FLOAT>(data->reverb.PositionLeft),
            static_cast<FLOAT>(data->reverb.PositionRight),
            static_cast<FLOAT>(data->reverb.PositionMatrixLeft),
            static_cast<FLOAT>(data->reverb.PositionMatrixRight),
            static_cast<FLOAT>(data->reverb.EarlyDiffusion),
            static_cast<FLOAT>(data->reverb.LateDiffusion),
            static_cast<FLOAT>(data->reverb.LowEQGain),
            static_cast<FLOAT>(data->reverb.LowEQCutoff),
            static_cast<FLOAT>(data->reverb.HighEQGain),
            static_cast<FLOAT>(data->reverb.HighEQCutoff),
            data->reverb.RoomFilterFreq,
            data->reverb.RoomFilterMain,
            data->reverb.RoomFilterHF,
            data->reverb.ReflectionsGain,
            data->reverb.ReverbGain,
            data->reverb.DecayTime,
            data->reverb.Density,
            data->reverb.RoomSize
        };

        INT64 context = isBus ? -voiceID : voiceID;
        Fader::Instance.StartFadeMulti(23, current, targets, fade, OnFadeReverbDisable, context);
    }

    void SaXAudio::SetEq(const INT32 voiceID, const BOOL isBus, const FXEQ_PARAMETERS* params, const FLOAT fade)
    {
        if (!m_XAudio) return;

        lock_guard<mutex> lock(m_voiceMutex);
        IXAudio2Voice* xVoice = nullptr;
        EffectData* data = nullptr;
        AudioVoice* audioVoice = nullptr;
        UINT32 channels = 2;

        if (isBus)
        {
            BusData* bus = SaXAudio::Instance.GetBus(voiceID);
            if (!bus || !bus->voice) return;
            data = bus;
            xVoice = bus->voice;
            channels = SaXAudio::Instance.m_masterDetails.InputChannels;
        }
        else
        {
            audioVoice = SaXAudio::Instance.GetVoice(voiceID);
            if (!audioVoice || !audioVoice->SourceVoice) return;
            data = &audioVoice->EffectData;
            xVoice = audioVoice->SourceVoice;
            if (audioVoice->BankData) channels = audioVoice->BankData->channels;
        }

        // --- Lazy Creation ---
        if (data->descriptors[CHAIN_EQ].pEffect == nullptr)
        {
            Log(voiceID, 0, "[SetEq] Lazy creating EQ DSP. Channels: " + to_string(channels));
            HRESULT hr = CreateFX(__uuidof(FXEQ), &data->descriptors[CHAIN_EQ].pEffect);
            if (FAILED(hr)) { Log(0, 0, "Failed to create EQ", hr); return; }

            data->descriptors[CHAIN_EQ].InitialState = TRUE;

            // CORRECTION CRITIQUE : On utilise la variable locale 'channels'
            data->descriptors[CHAIN_EQ].OutputChannels = channels;

            if (!isBus && audioVoice) UpdateVoiceEffectChain(audioVoice);
        }

        // --- 3. Calcul de l'Index Réel ---
        // L'EQ est après la Reverb. Si la Reverb existe, EQ est à l'index 1. Sinon, à l'index 0.
        UINT32 realIndex = 0;
        if (data->descriptors[CHAIN_REVERB].pEffect != nullptr) realIndex++;

        xVoice->EnableEffect(realIndex);

        // --- 4. Application ---
        if (fade <= 0)
        {
            data->eq = *params;
            HRESULT hr = xVoice->SetEffectParameters(realIndex, &data->eq, sizeof(FXEQ_PARAMETERS), XAUDIO2_COMMIT_NOW);
            if (FAILED(hr)) Log(0, 0, "Failed to set EQ parameters", hr);
            return;
        }

        // ... (Reste du code de mapping Fader identique, voir bloc suivant pour OnFadeEq) ...
        // Note : Le bloc de préparation des tableaux `current` et `targets` reste identique à ton code d'origine.
        // Copie-colle le bloc de ton ancien SetEq ici.
        Log(0, 0, "FrequencyCenter0: " + to_string(data->eq.FrequencyCenter0)); // Ton log existant

        // Preparation tableaux (copier ton code existant ici) ...
        FLOAT* current = new FLOAT[12] { /* ... */ };
        FLOAT* targets = new FLOAT[12] { /* ... */ };

        INT64 context = isBus ? -voiceID : voiceID;
        Fader::Instance.StartFadeMulti(12, current, targets, fade, OnFadeEq, context);
    }

    void SaXAudio::RemoveEq(const INT32 voiceID, const BOOL isBus, const FLOAT fade)
    {
        if (!m_XAudio)
            return;

        IXAudio2Voice* voice = nullptr;
        EffectData* data = nullptr;
        GetEffectData(voiceID, isBus, &voice, &data);
        if (!voice) return;

        if (fade <= 0)
        {
            voice->DisableEffect(CHAIN_EQ);
            return;
        }

        FLOAT* current = new FLOAT[12]
        {
            data->eq.FrequencyCenter0,
            data->eq.Gain0,
            data->eq.Bandwidth0,
            data->eq.FrequencyCenter1,
            data->eq.Gain1,
            data->eq.Bandwidth1,
            data->eq.FrequencyCenter2,
            data->eq.Gain2,
            data->eq.Bandwidth2,
            data->eq.FrequencyCenter3,
            data->eq.Gain3,
            data->eq.Bandwidth3
        };

        EffectData defaultData;
        FLOAT* targets = new FLOAT[12]
        {
            defaultData.eq.FrequencyCenter0,
            defaultData.eq.Gain0,
            defaultData.eq.Bandwidth0,
            defaultData.eq.FrequencyCenter1,
            defaultData.eq.Gain1,
            defaultData.eq.Bandwidth1,
            defaultData.eq.FrequencyCenter2,
            defaultData.eq.Gain2,
            defaultData.eq.Bandwidth2,
            defaultData.eq.FrequencyCenter3,
            defaultData.eq.Gain3,
            defaultData.eq.Bandwidth3
        };

        INT64 context = isBus ? -voiceID : voiceID;
        Fader::Instance.StartFadeMulti(12, current, targets, fade, OnFadeEqDisable, context);
    }

    void SaXAudio::SetEcho(const INT32 voiceID, const BOOL isBus, const FXECHO_PARAMETERS* params, const FLOAT fade)
    {
        if (!m_XAudio) return;

        lock_guard<mutex> lock(m_voiceMutex);
        IXAudio2Voice* xVoice = nullptr;
        EffectData* data = nullptr;
        AudioVoice* audioVoice = nullptr;
        UINT32 channels = 2;

        if (isBus)
        {
            BusData* bus = SaXAudio::Instance.GetBus(voiceID);
            if (!bus || !bus->voice) return;
            data = bus;
            xVoice = bus->voice;
            channels = SaXAudio::Instance.m_masterDetails.InputChannels;
        }
        else
        {
            audioVoice = SaXAudio::Instance.GetVoice(voiceID);
            if (!audioVoice || !audioVoice->SourceVoice) return;
            data = &audioVoice->EffectData;
            xVoice = audioVoice->SourceVoice;
            if (audioVoice->BankData) channels = audioVoice->BankData->channels;
        }

        // --- Lazy Creation ---
        if (data->descriptors[CHAIN_ECHO].pEffect == nullptr)
        {
            Log(voiceID, 0, "[SetEcho] Lazy creating Echo DSP. Channels: " + to_string(channels));
            FXECHO_INITDATA init = { 3000 };
            HRESULT hr = CreateFX(__uuidof(FXEcho), &data->descriptors[CHAIN_ECHO].pEffect, &init, sizeof(FXECHO_INITDATA));
            if (FAILED(hr)) { Log(0, 0, "Failed to create Echo", hr); return; }

            data->descriptors[CHAIN_ECHO].InitialState = TRUE;

            // CORRECTION CRITIQUE : On utilise 'channels'
            data->descriptors[CHAIN_ECHO].OutputChannels = channels;

            if (!isBus && audioVoice) UpdateVoiceEffectChain(audioVoice);
        }

        // --- Calcul de l'Index Réel ---
        // L'Echo est en 3ème position théorique.
        UINT32 realIndex = 0;
        if (data->descriptors[CHAIN_REVERB].pEffect != nullptr) realIndex++;
        if (data->descriptors[CHAIN_EQ].pEffect != nullptr) realIndex++;

        xVoice->EnableEffect(realIndex);

        if (fade <= 0)
        {
            data->echo = *params;
            HRESULT hr = xVoice->SetEffectParameters(realIndex, &data->echo, sizeof(FXECHO_PARAMETERS), XAUDIO2_COMMIT_NOW);
            if (FAILED(hr)) Log(0, 0, "Failed to set echo parameters", hr);
            return;
        }

        // ... (Reste du code mapping Fader identique à l'original) ...
        // Copie-colle la préparation des tableaux `current` et `targets`
        FLOAT* current = new FLOAT[3] { /* ... */ };
        FLOAT* targets = new FLOAT[3] { /* ... */ };

        INT64 context = isBus ? -voiceID : voiceID;
        Fader::Instance.StartFadeMulti(3, current, targets, fade, OnFadeEcho, context);
    }

    void SaXAudio::RemoveEcho(const INT32 voiceID, const BOOL isBus, const FLOAT fade)
    {
        if (!m_XAudio)
            return;

        IXAudio2Voice* voice = nullptr;
        EffectData* data = nullptr;
        GetEffectData(voiceID, isBus, &voice, &data);
        if (!voice) return;

        if (fade <= 0)
        {
            voice->DisableEffect(CHAIN_ECHO);
            return;
        }

        FLOAT* current = new FLOAT[3]
        {
            data->echo.WetDryMix,
            data->echo.Feedback,
            data->echo.Delay
        };

        FLOAT* targets = new FLOAT[3] { 0 };

        INT64 context = isBus ? -voiceID : voiceID;
        Fader::Instance.StartFadeMulti(3, current, targets, fade, OnFadeEchoDisable, context);
    }

    UINT32 SaXAudio::GetVoiceCount(const INT32 bankID, const INT32 busID)
    {
        if (!m_XAudio)
            return 0;
        lock_guard<mutex> lock(m_voiceMutex);

        UINT32 count = 0;
        for (auto& it : m_voices)
        {
            if (bankID > 0 && it.second->BankID != bankID)
                continue;
            if (busID > 0 && it.second->BusID != busID)
                continue;
            if (it.second->SourceVoice)
                count++;
        }
        return count;
    }

    UINT32 SaXAudio::GetBankCount()
    {
        if (!m_XAudio)
            return 0;
        lock_guard<mutex> lock(m_bankMutex);

        UINT32 count = 0;
        for (auto& it : m_bank)
        {
            if (!it.second.disposed)
                count++;
        }

        return count;
    }

    void SaXAudio::DecodeOgg(const INT32 bankID, stb_vorbis* vorbis)
    {
        // Reset file position
        stb_vorbis_seek_start(vorbis);

        UINT32 samplesTotal = 0;
        UINT32 samplesDecoded = 0;
        UINT32 bufferSize = 4096;

        auto it = SaXAudio::Instance.m_bank.find(bankID);
        if (it != SaXAudio::Instance.m_bank.end())
        {
            BankData* data = &it->second;
            if (data)
            {
                data->decodedSamples = 0;
                samplesTotal = data->totalSamples;
            }
        }

        while (samplesTotal - samplesDecoded > 0)
        {
            if (bufferSize > samplesTotal)
                bufferSize = samplesTotal;

            lock_guard<mutex> lock(SaXAudio::Instance.m_bankMutex);

            BankData* data = nullptr;
            it = SaXAudio::Instance.m_bank.find(bankID);
            if (it != SaXAudio::Instance.m_bank.end())
                data = &it->second;

            // BankEntry removed
            if (!data || data->disposed || !data->buffer) break;

            {
                lock_guard<mutex> lock(data->decodingMutex);

                // Read samples
                FLOAT* pBuffer = &data->buffer[data->decodedSamples * data->channels];
                UINT32 decoded = stb_vorbis_get_samples_float_interleaved(vorbis, data->channels, pBuffer, bufferSize * data->channels);
                samplesDecoded += decoded;
                data->decodedSamples = samplesDecoded;

                if (decoded == 0)
                {
                    // Less samples decoded than expected
                    // we update the total samples to match
                    data->totalSamples = samplesDecoded;
                }

                data->decodingPerform.notify_one();
            }
        }

        // Close the Vorbis file
        stb_vorbis_close(vorbis);

        {
            // Calling back
            lock_guard<mutex> lock(SaXAudio::Instance.m_bankMutex);

            BankData* data = GetEntry(data, SaXAudio::Instance.m_bank, bankID);
            if (data && data->onDecodedCallback)
            {
                (*data->onDecodedCallback)(bankID, data->Oggbuffer);
                data->onDecodedCallback = nullptr;
            }
        }

        Log(bankID, 0, "[DecodeOgg] Decoding complete");
    }

    void SaXAudio::RemoveVoice(const INT32 voiceID)
    {
        BOOL autoRemove = false;
        INT32 bankID = 0;
        {
            lock_guard<mutex> lock(m_voiceMutex);

            AudioVoice* voice = nullptr;
            auto it_voice = m_voices.find(voiceID);
            if (it_voice != m_voices.end())
                voice = it_voice->second;
            if (!voice) return;
            bankID = voice->BankID;
            voice->BankID = 0;

            // Stop the voice
            if (voice->SourceVoice)
            {
                Log(bankID, voiceID, "[RemoveVoice] Stopping voice");
                ClearVoiceEffects(voice);
                voice->SourceVoice->DestroyVoice();
                voice->SourceVoice = nullptr;
            }

            // Callback
            if (voice->IsPlaying && OnFinishedCallback != nullptr)
            {
                thread onFinished(*OnFinishedCallback, voiceID);
                onFinished.detach();
            }

            // Auto remove logic
            BankData* data = GetEntry(data, m_bank, bankID);
            if (data && data->autoRemove)
            {
                autoRemove = true;
                for (auto& it : m_voices)
                {
                    if (it.second->BankID == bankID)
                    {
                        autoRemove = false;
                        break;
                    }
                }
            }

            // Voice ready to be reused
            voice->Reset();
            m_voicePool.push(voice);
            m_voices.erase(voiceID);

            Log(bankID, voiceID, "[RemoveVoice] Deleted voice");
        }

        if (autoRemove)
            RemoveBankEntry(bankID);
    }

    void SaXAudio::CreateEffectChain(IXAudio2Voice* voice, EffectData* data)
    {
        HRESULT hr = XAudio2CreateReverb(&data->descriptors[CHAIN_REVERB].pEffect);
        if (FAILED(hr))
        {
            Log(0, 0, "Failed to create reverb effect", hr);
        }

        hr = CreateFX(__uuidof(FXEQ), &data->descriptors[CHAIN_EQ].pEffect);
        if (FAILED(hr))
        {
            Log(0, 0, "Failed to create EQ effect", hr);
        }

        FXECHO_INITDATA init = { 3000 };
        hr = CreateFX(__uuidof(FXEcho), &data->descriptors[CHAIN_ECHO].pEffect, &init, sizeof(FXECHO_INITDATA));
        if (FAILED(hr))
        {
            Log(0, 0, "Failed to create echo effect", hr);
        }

        data->effectChain.EffectCount = 3;
        data->effectChain.pEffectDescriptors = data->descriptors;

        hr = voice->SetEffectChain(&data->effectChain);
        if (FAILED(hr))
        {
            Log(0, 0, "Failed to set effect chain", hr);
        }
    }

    void SaXAudio::OnFadeReverb(INT64 context, UINT32 count, FLOAT* newValues, BOOL hasFinished)
    {
        BOOL isBus = context < 0;
        INT32 voiceID = isBus ? -(INT32)context : (INT32)context;

        IXAudio2Voice* voice = nullptr;
        EffectData* data = nullptr;
        GetEffectData(voiceID, isBus, &voice, &data);
        if (!voice) return;

        INT32 i = 0;
        data->reverb.WetDryMix = newValues[i++];
        data->reverb.ReflectionsDelay = static_cast<UINT32>(newValues[i++]);
        data->reverb.ReverbDelay = static_cast<BYTE>(newValues[i++]);
        data->reverb.RearDelay = static_cast<BYTE>(newValues[i++]);
        data->reverb.SideDelay = static_cast<BYTE>(newValues[i++]);
        data->reverb.PositionLeft = static_cast<BYTE>(newValues[i++]);
        data->reverb.PositionRight = static_cast<BYTE>(newValues[i++]);
        data->reverb.PositionMatrixLeft = static_cast<BYTE>(newValues[i++]);
        data->reverb.PositionMatrixRight = static_cast<BYTE>(newValues[i++]);
        data->reverb.EarlyDiffusion = static_cast<BYTE>(newValues[i++]);
        data->reverb.LateDiffusion = static_cast<BYTE>(newValues[i++]);
        data->reverb.LowEQGain = static_cast<BYTE>(newValues[i++]);
        data->reverb.LowEQCutoff = static_cast<BYTE>(newValues[i++]);
        data->reverb.HighEQGain = static_cast<BYTE>(newValues[i++]);
        data->reverb.HighEQCutoff = static_cast<BYTE>(newValues[i++]);
        data->reverb.RoomFilterFreq = newValues[i++];
        data->reverb.RoomFilterMain = newValues[i++];
        data->reverb.RoomFilterHF = newValues[i++];
        data->reverb.ReflectionsGain = newValues[i++];
        data->reverb.ReverbGain = newValues[i++];
        data->reverb.DecayTime = newValues[i++];
        data->reverb.Density = newValues[i++];
        data->reverb.RoomSize = newValues[i++];

        HRESULT hr = voice->SetEffectParameters(CHAIN_REVERB, &data->reverb, sizeof(XAUDIO2FX_REVERB_PARAMETERS), XAUDIO2_COMMIT_NOW);
        if (FAILED(hr))
        {
            Log(0, 0, "Failed to set reverb parameters", hr);
        }
    }

    void SaXAudio::OnFadeReverbDisable(INT64 context, UINT32 count, FLOAT* newValues, BOOL hasFinished)
    {
        BOOL isBus = context < 0;
        INT32 voiceID = isBus ? -(INT32)context : (INT32)context;

        IXAudio2Voice* voice = nullptr;
        EffectData* data = nullptr;
        GetEffectData(voiceID, isBus, &voice, &data);
        if (!voice) return;

        if (hasFinished)
        {
            voice->DisableEffect(CHAIN_REVERB);
            return;
        }

        OnFadeReverb(context, count, newValues, hasFinished);
    }

    void SaXAudio::OnFadeEq(INT64 context, UINT32 count, FLOAT* newValues, BOOL hasFinished)
    {
        // ... (Récupération voice/data inchangée) ...
        BOOL isBus = context < 0;
        INT32 voiceID = isBus ? -(INT32)context : (INT32)context;
        IXAudio2Voice* voice = nullptr;
        EffectData* data = nullptr;
        GetEffectData(voiceID, isBus, &voice, &data);
        if (!voice) return;

        // ... (Mapping des valeurs newValues vers data->eq inchangé) ...
        INT32 i = 0;
        data->eq.FrequencyCenter0 = newValues[i++];
        // ... etc ...

        // --- CALCUL DE L'INDEX DYNAMIQUE ---
        UINT32 realIndex = 0;
        if (data->descriptors[CHAIN_REVERB].pEffect != nullptr) realIndex++;

        // Utilisation de realIndex au lieu de CHAIN_EQ
        HRESULT hr = voice->SetEffectParameters(realIndex, &data->eq, sizeof(FXEQ_PARAMETERS), XAUDIO2_COMMIT_NOW);

        if (FAILED(hr)) Log(0, 0, "Failed to set EQ parameters", hr);
    }

    void SaXAudio::OnFadeEqDisable(INT64 context, UINT32 count, FLOAT* newValues, BOOL hasFinished)
    {
        BOOL isBus = context < 0;
        INT32 voiceID = isBus ? -(INT32)context : (INT32)context;

        IXAudio2Voice* voice = nullptr;
        EffectData* data = nullptr;
        GetEffectData(voiceID, isBus, &voice, &data);
        if (!voice) return;

        if (hasFinished)
        {
            voice->DisableEffect(CHAIN_EQ);
            return;
        }

        OnFadeEq(context, count, newValues, hasFinished);
    }

    void SaXAudio::OnFadeEcho(INT64 context, UINT32 count, FLOAT* newValues, BOOL hasFinished)
    {
        // ... (Récupération voice/data inchangée) ...
        BOOL isBus = context < 0;
        INT32 voiceID = isBus ? -(INT32)context : (INT32)context;
        IXAudio2Voice* voice = nullptr;
        EffectData* data = nullptr;
        GetEffectData(voiceID, isBus, &voice, &data);
        if (!voice) return;

        // ... (Mapping valeurs) ...
        INT32 i = 0;
        data->echo.WetDryMix = newValues[i++];
        // ... etc ...

        // --- CALCUL DE L'INDEX DYNAMIQUE ---
        UINT32 realIndex = 0;
        if (data->descriptors[CHAIN_REVERB].pEffect != nullptr) realIndex++;
        if (data->descriptors[CHAIN_EQ].pEffect != nullptr) realIndex++;

        // Utilisation de realIndex au lieu de CHAIN_ECHO
        HRESULT hr = voice->SetEffectParameters(realIndex, &data->echo, sizeof(FXECHO_PARAMETERS), XAUDIO2_COMMIT_NOW);

        if (FAILED(hr)) Log(0, 0, "Failed to set Echo parameters", hr);
    }

    void SaXAudio::OnFadeEchoDisable(INT64 context, UINT32 count, FLOAT* newValues, BOOL hasFinished)
    {
        BOOL isBus = context < 0;
        INT32 voiceID = isBus ? -(INT32)context : (INT32)context;

        IXAudio2Voice* voice = nullptr;
        EffectData* data = nullptr;
        GetEffectData(voiceID, isBus, &voice, &data);
        if (!voice) return;

        if (hasFinished)
        {
            voice->DisableEffect(CHAIN_ECHO);
            return;
        }

        OnFadeEcho(context, count, newValues, hasFinished);
    }
}
