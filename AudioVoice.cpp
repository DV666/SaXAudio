// MIT License
// ... (Header license inchangé)

#include "SaXAudio.h"
#include "Fader.h"

namespace SaXAudio
{
    BOOL AudioVoice::Start(const UINT32 atSample, BOOL flush)
    {
        if (!SourceVoice || !BankData) return false;
        Log(BankID, VoiceID, "[Start] at: " + to_string(atSample) + (Looping ? " loop start: " + to_string(LoopStart) + " loop end: " + to_string(LoopEnd) : ""));

        m_positionOffset = atSample;
        if (IsPlaying)
        {
            if (flush)
            {
                m_tempFlush++;
                SourceVoice->Stop();
                SourceVoice->FlushSourceBuffers();
            }
            XAUDIO2_VOICE_STATE state;
            SourceVoice->GetState(&state);
            m_positionOffset -= state.SamplesPlayed;
        }

        Buffer.PlayBegin = atSample;
        Buffer.PlayLength = 0; 

        if (Looping && atSample < LoopEnd)
        {
            Buffer.LoopBegin = LoopStart;
            Buffer.LoopLength = LoopEnd - LoopStart;
            Buffer.LoopCount = XAUDIO2_LOOP_INFINITE;
        }
        else
        {
            Looping = false;
            Buffer.LoopBegin = 0;
            Buffer.LoopLength = 0;
            Buffer.LoopCount = 0;
        }

        HRESULT hr = SourceVoice->SubmitSourceBuffer(&Buffer);
        if (FAILED(hr))
        {
            Log(BankID, VoiceID, "[Start] Failed to submit buffer", hr);
            SaXAudio::Instance.RemoveVoice(VoiceID);
            return false;
        }

        IsPlaying = true;

        if (m_pauseStack > 0)
        {
            Log(BankID, VoiceID, "[Start] Voice paused");
            return IsPlaying;
        }

        if (BankData->decodedSamples <= atSample)
        {
            // Waiting for some decoded samples to not read garbage
            // Fix: Passage par ID pour thread safety
            thread wait(WaitForDecoding, VoiceID, BankID, Buffer.PlayBegin);
            wait.detach();
        }
        else if (FAILED(hr = SourceVoice->Start()))
        {
            Log(BankID, VoiceID, "[Start] FAILED starting", hr);
            SaXAudio::Instance.RemoveVoice(VoiceID);
            return false;
        }
        return IsPlaying;
    }

    // --- FONCTION CORRIGÉE ET SÉCURISÉE ---
    void AudioVoice::WaitForDecoding(INT32 voiceID, INT32 bankID, UINT32 playBegin)
    {
        Log(bankID, voiceID, "[Start] Waiting for decoded data");

        // --- CORRECTION ICI : On ajoute 'struct' pour désigner le TYPE ---
        struct BankData* pBankData = nullptr;
        // ---------------------------------------------------------------

        // 2. Récupération sécurisée du pointeur
        {
            lock_guard<mutex> lock(SaXAudio::Instance.m_voiceMutex);

            auto it = SaXAudio::Instance.m_voices.find(voiceID);
            if (it != SaXAudio::Instance.m_voices.end())
            {
                AudioVoice* voice = it->second;
                // On vérifie que la voix correspond bien toujours à la banque demandée
                if (voice && voice->BankID == bankID)
                {
                    pBankData = voice->BankData;
                }
            }
        }

        // Si la banque n'a pas été trouvée ou la voix supprimée, on sort.
        if (!pBankData) return;

        // 3. Attente (Lock sur la banque uniquement)
        {
            unique_lock<mutex> bankLock(pBankData->decodingMutex);

            if (pBankData->decodedSamples == 0) // Si pas encore décodé
            {
                // On attend que le décodage atteigne le point de lecture
                BOOL result = pBankData->decodingPerform.wait_for(
                    bankLock,
                    chrono::milliseconds(500),
                    [pBankData, playBegin] { return pBankData->decodedSamples > playBegin; }
                );

                if (!result)
                {
                    Log(bankID, voiceID, " ERROR | [Start] Failed waiting for decoded data, timed out");
                    return;
                }
            }
        }

        // 4. Démarrage (Lock Global pour éviter conflit avec SetReverb/SetEcho)
        {
            lock_guard<mutex> lock(SaXAudio::Instance.m_voiceMutex);

            // On revérifie tout car du temps a passé
            auto it = SaXAudio::Instance.m_voices.find(voiceID);
            if (it == SaXAudio::Instance.m_voices.end()) return;

            AudioVoice* voice = it->second;
            if (!voice || voice->BankID != bankID || !voice->SourceVoice) return;

            if (voice->GetPauseStack() > 0) return;

            HRESULT hr = voice->SourceVoice->Start();

            if (FAILED(hr))
            {
                Log(bankID, voiceID, "[Start] Failed starting (Async)", hr);
            }
            else
            {
                // voice->m_tempFlush = 0; 
                Log(bankID, voiceID, "[Start] Successfully waited for decoded data");
            }
        }
    }

    // ... (Le reste du fichier : Stop, Pause, Resume... reste inchangé) ...
    BOOL AudioVoice::Stop(const FLOAT fade)
    {
        if (!SourceVoice || !IsPlaying) return false;
        Log(BankID, VoiceID, "[Stop] fade: " + to_string(fade));

        IsPlaying = false;
        Looping = false;

        Fader::Instance.StopFade(m_volumeFadeID);
        m_volumeFadeID = 0;

        if (fade > 0)
        {
            FLOAT current = 1.0f;
            SourceVoice->GetVolume(&current);
            m_volumeFadeID = Fader::Instance.StartFade(current, 0, fade, OnFadeVolume, VoiceID);
            if (m_pauseStack > 0)
                Fader::Instance.PauseFade(m_volumeFadeID);
        }
        else
        {
            m_tempFlush = 0;
            SourceVoice->Stop();
            SourceVoice->FlushSourceBuffers();
        }
        return true;
    }

    UINT32 AudioVoice::Pause(const FLOAT fade)
    {
        if (!SourceVoice) return 0;

        m_pauseStack++;
        Log(BankID, VoiceID, "[Pause] stack: " + to_string(m_pauseStack));

        Fader::Instance.StopFade(m_pauseFadeID);
        m_pauseFadeID = 0;

        Fader::Instance.PauseFade(m_volumeFadeID);
        Fader::Instance.PauseFade(m_speedFadeID);
        Fader::Instance.PauseFade(m_panningFadeID);

        if (fade > 0 && Volume > 0 && IsPlaying)
        {
            FLOAT current = 1.0f;
            SourceVoice->GetVolume(&current);
            m_pauseFadeID = Fader::Instance.StartFade(current, 0, fade, OnFadeVolume, VoiceID);
        }
        else
        {
            SourceVoice->Stop();
        }
        return m_pauseStack;
    }

    UINT32 AudioVoice::Resume(const FLOAT fade)
    {
        if (!SourceVoice || m_pauseStack == 0) return 0;

        m_pauseStack--;
        Log(BankID, VoiceID, "[Resume] stack: " + to_string(m_pauseStack) + (Looping ? " - Looping" : ""));

        if (m_pauseStack > 0)
            return m_pauseStack;

        SourceVoice->Start();

        Fader::Instance.StopFade(m_pauseFadeID);
        m_pauseFadeID = 0;

        if (fade > 0 && Volume > 0)
        {
            FLOAT current = 0.0f;
            SourceVoice->GetVolume(&current);
            m_pauseFadeID = Fader::Instance.StartFade(current, Volume, fade, OnFadeVolume, VoiceID);
        }
        else
        {
            SourceVoice->SetVolume(Volume);
            Fader::Instance.ResumeFade(m_volumeFadeID);
            Fader::Instance.ResumeFade(m_speedFadeID);
            Fader::Instance.ResumeFade(m_panningFadeID);
        }
        return m_pauseStack;
    }

    UINT32 AudioVoice::GetPauseStack()
    {
        return m_pauseStack;
    }

    UINT64 AudioVoice::CalculateCurrentPosition()
    {
        XAUDIO2_VOICE_STATE state;
        SourceVoice->GetState(&state);

        UINT64 position = state.SamplesPlayed + m_positionOffset;
        if (Looping && position > LoopStart)
        {
            position = LoopStart + ((position - LoopStart) % (UINT64)(LoopEnd - LoopStart));
        }
        return position;
    }

    UINT32 AudioVoice::GetPosition()
    {
        if (!IsPlaying || !SourceVoice) return 0;

        UINT64 position = CalculateCurrentPosition();
        if (position == 0) return 1; 
        return (UINT32)position;
    }

    void AudioVoice::ChangeLoopPoints(const UINT32 start, UINT32 end)
    {
        if (!SourceVoice || !BankData) return;
        Log(BankID, VoiceID, "[ChangeLoopPoints] start: " + to_string(start) + " end: " + to_string(end));

        if (end == 0)
            end = BankData->totalSamples - 1;

        if (LoopStart == start && LoopEnd == end)
            return;

        UINT64 position = 0;
        if (IsPlaying)
        {
            SourceVoice->Stop();
            position = CalculateCurrentPosition();
            m_tempFlush++;
            SourceVoice->FlushSourceBuffers();
        }

        if (start < end)
        {
            LoopStart = start;
            LoopEnd = end;
        }
        else
        {
            LoopStart = end;
            LoopEnd = start;
        }

        if (LoopStart == LoopEnd && LoopEnd != 0)
            LoopStart = LoopEnd - 1;

        if (IsPlaying)
            Start((UINT32)position, false);
    }

    void AudioVoice::SetLooping(BOOL state)
    {
        if (!SourceVoice || !BankData || Looping == state) return;
        Log(BankID, VoiceID, "[SetLooping] " + to_string(state));

        UINT64 position = 0;
        if (IsPlaying)
        {
            SourceVoice->Stop();
            position = CalculateCurrentPosition();
            m_tempFlush++;
            SourceVoice->FlushSourceBuffers();
        }

        Looping = state; 

        if (Looping && LoopEnd == 0)
            LoopEnd = BankData->totalSamples - 1;

        if (IsPlaying)
            Start((UINT32)position, false);
    }

    void AudioVoice::SetVolume(const FLOAT volume, const FLOAT fade)
    {
        if (!SourceVoice || Volume == volume) return;
        Log(BankID, VoiceID, "[SetVolume] to: " + to_string(volume) + " fade: " + to_string(fade));

        Fader::Instance.StopFade(m_volumeFadeID);
        m_volumeFadeID = 0;

        if (fade > 0)
        {
            FLOAT current = 1.0f;
            SourceVoice->GetVolume(&current);
            m_volumeFadeID = Fader::Instance.StartFade(current, volume, fade, OnFadeVolume, VoiceID);
            if (m_pauseStack > 0)
                Fader::Instance.PauseFade(m_volumeFadeID);
        }
        else
        {
            SourceVoice->SetVolume(volume);
        }
        Volume = volume;
    }

    void AudioVoice::SetSpeed(FLOAT speed, const FLOAT fade)
    {
        if (!SourceVoice || !BankData || Speed == speed) return;
        Log(BankID, VoiceID, "[SetSpeed] to: " + to_string(speed) + " fade: " + to_string(fade));

        if (speed < XAUDIO2_MIN_FREQ_RATIO)
            speed = XAUDIO2_MIN_FREQ_RATIO;

        Fader::Instance.StopFade(m_speedFadeID);
        m_speedFadeID = 0;

        if (fade > 0)
        {
            m_speedFadeID = Fader::Instance.StartFade(Speed, speed, fade, OnFadeSpeed, VoiceID);
            if (m_pauseStack > 0)
                Fader::Instance.PauseFade(m_speedFadeID);
        }
        else
        {
            Speed = speed;
            SourceVoice->SetFrequencyRatio(speed);
        }
    }

    void AudioVoice::SetPanning(const FLOAT panning, const FLOAT fade)
    {
        if (!SourceVoice || Panning == panning) return;
        Log(BankID, VoiceID, "[SetPanning] to: " + to_string(panning) + " fade: " + to_string(fade));

        Fader::Instance.StopFade(m_panningFadeID);
        m_panningFadeID = 0;

        if (fade > 0)
        {
            m_panningFadeID = Fader::Instance.StartFade(Panning, panning, fade, OnFadePanning, VoiceID);
            if (m_pauseStack > 0)
                Fader::Instance.PauseFade(m_panningFadeID);
        }
        else
        {
            Panning = panning;
            SetOutputMatrix(panning);
        }
    }

    void AudioVoice::SetOutputMatrix(FLOAT panning)
    {
        if (!SourceVoice || !BankData) return;
        // (Code Panning inchangé ...)
        UINT32 sourceChannels = BankData->channels;
        UINT32 destChannels = SaXAudio::Instance.m_masterDetails.InputChannels;
        DWORD channelMask = SaXAudio::Instance.m_channelMask;
        if (sourceChannels > 2 || destChannels == 0) return;

        const UINT32 inChannels = 2;
        const UINT32 outChannels = 12;
        FLOAT outputMatrix[inChannels * outChannels] = { 0 };

        INT32 leftChannelIndex = -1;
        INT32 rightChannelIndex = -1;
        INT32 centerChannelIndex = -1;
        INT32 lfeChannelIndex = -1;
        INT32 backLeftChannelIndex = -1;
        INT32 backRightChannelIndex = -1;
        INT32 sideLeftChannelIndex = -1;
        INT32 sideRightChannelIndex = -1;

        DWORD currentMask = 1;
        INT32 channelIndex = 0;

        for (UINT32 i = 0; i < outChannels && channelIndex < (INT32)destChannels; i++)
        {
            if (channelMask & currentMask)
            {
                switch (currentMask)
                {
                case SPEAKER_FRONT_LEFT: leftChannelIndex = channelIndex; break;
                case SPEAKER_FRONT_RIGHT: rightChannelIndex = channelIndex; break;
                case SPEAKER_FRONT_CENTER: centerChannelIndex = channelIndex; break;
                case SPEAKER_LOW_FREQUENCY: lfeChannelIndex = channelIndex; break;
                case SPEAKER_BACK_LEFT: backLeftChannelIndex = channelIndex; break;
                case SPEAKER_BACK_RIGHT: backRightChannelIndex = channelIndex; break;
                case SPEAKER_SIDE_LEFT: sideLeftChannelIndex = channelIndex; break;
                case SPEAKER_SIDE_RIGHT: sideRightChannelIndex = channelIndex; break;
                }
                channelIndex++;
            }
            currentMask <<= 1;
        }

        if (sourceChannels == 1)
        {
            FLOAT leftGain = min(1, 1 - panning);
            FLOAT rightGain = min(1, 1 + panning);
            if (leftChannelIndex >= 0) outputMatrix[leftChannelIndex] = leftGain;
            if (rightChannelIndex >= 0) outputMatrix[rightChannelIndex] = rightGain;
            if (centerChannelIndex >= 0) outputMatrix[centerChannelIndex] = 0.707f;
            FLOAT surroundGain = 0.5f; 
            if (backLeftChannelIndex >= 0) outputMatrix[backLeftChannelIndex] = surroundGain * leftGain;
            if (backRightChannelIndex >= 0) outputMatrix[backRightChannelIndex] = surroundGain * rightGain;
            if (sideLeftChannelIndex >= 0) outputMatrix[sideLeftChannelIndex] = surroundGain * leftGain;
            if (sideRightChannelIndex >= 0) outputMatrix[sideRightChannelIndex] = surroundGain * rightGain;
        }
        else if (sourceChannels == 2)
        {
            FLOAT LR = max(0, -panning);
            FLOAT RL = max(0, panning);
            FLOAT LL = min(1, 1 - panning);
            FLOAT RR = min(1, 1 + panning);

            if (leftChannelIndex >= 0) { outputMatrix[leftChannelIndex * sourceChannels + 0] = LL; outputMatrix[leftChannelIndex * sourceChannels + 1] = LR; }
            if (rightChannelIndex >= 0) { outputMatrix[rightChannelIndex * sourceChannels + 0] = RL; outputMatrix[rightChannelIndex * sourceChannels + 1] = RR; }
            if (centerChannelIndex >= 0) { outputMatrix[centerChannelIndex * sourceChannels + 0] = 0.707f; outputMatrix[centerChannelIndex * sourceChannels + 1] = 0.707f; }

            FLOAT surroundGain = 0.5f;
            if (backLeftChannelIndex >= 0) { outputMatrix[backLeftChannelIndex * sourceChannels + 0] = surroundGain * LL; outputMatrix[backLeftChannelIndex * sourceChannels + 1] = surroundGain * LR; }
            if (backRightChannelIndex >= 0) { outputMatrix[backRightChannelIndex * sourceChannels + 0] = surroundGain * RL; outputMatrix[backRightChannelIndex * sourceChannels + 1] = surroundGain * RR; }
            if (sideLeftChannelIndex >= 0) { outputMatrix[sideLeftChannelIndex * sourceChannels + 0] = surroundGain * LL; outputMatrix[sideLeftChannelIndex * sourceChannels + 1] = surroundGain * LR; }
            if (sideRightChannelIndex >= 0) { outputMatrix[sideRightChannelIndex * sourceChannels + 0] = surroundGain * RL; outputMatrix[sideRightChannelIndex * sourceChannels + 1] = surroundGain * RR; }
        }

        HRESULT hr = SourceVoice->SetOutputMatrix(nullptr, sourceChannels, destChannels, outputMatrix);
        if (FAILED(hr))
        {
            Log(BankID, VoiceID, "[SetOutputMatrix] Failed.", hr);
        }
    }

    void AudioVoice::Reset()
    {
        Fader::Instance.StopFade(m_volumeFadeID);
        Fader::Instance.StopFade(m_speedFadeID);
        Fader::Instance.StopFade(m_panningFadeID);
        Fader::Instance.StopFade(m_pauseFadeID);

        m_volumeFadeID = 0; m_speedFadeID = 0; m_panningFadeID = 0; m_pauseFadeID = 0;
        m_pauseStack = 0; m_positionOffset = 0; m_volumeTarget = 0; m_tempFlush = 0;

        if (SourceVoice)
        {
            for (int i = 0; i < 3; i++) SourceVoice->DisableEffect(i);
        }

        EffectData.effectChain.EffectCount = 0;
        EffectData.effectChain.pEffectDescriptors = nullptr;

        Buffer = { 0 };
        BankID = 0; BusID = 0;
        SourceVoice = nullptr; BankData = nullptr;
        Volume = 1.0f; Speed = 1.0f; Panning = 0.0f;
        LoopStart = 0; LoopEnd = 0; Looping = false; IsPlaying = false;
    }

    void AudioVoice::OnFadeVolume(INT64 voiceID, UINT32 count, FLOAT* newValues, BOOL hasFinished)
    {
        AudioVoice* voice = SaXAudio::Instance.GetVoice((INT32)voiceID);
        if (!voice) return;
        voice->SourceVoice->SetVolume(newValues[0]);

        if (hasFinished)
        {
            if (voice->m_pauseFadeID != 0)
            {
                voice->m_pauseFadeID = 0;
                if (voice->m_pauseStack > 0)
                {
                    voice->SourceVoice->Stop();
                    Log(voice->BankID, voice->VoiceID, "[OnFadeVolume] Pause");
                }
                else
                {
                    Fader::Instance.ResumeFade(voice->m_volumeFadeID);
                    Fader::Instance.ResumeFade(voice->m_speedFadeID);
                    Fader::Instance.ResumeFade(voice->m_panningFadeID);
                    Log(voice->BankID, voice->VoiceID, "[OnFadeVolume] Resume");
                }
            }
            else
            {
                voice->m_volumeFadeID = 0;
            }

            if (!voice->IsPlaying)
            {
                voice->m_tempFlush = 0;
                voice->SourceVoice->Stop();
                voice->SourceVoice->FlushSourceBuffers();
                Log(voice->BankID, voice->VoiceID, "[OnFadeVolume] Stop");
            }
        }
    }

    void AudioVoice::OnFadeSpeed(INT64 voiceID, UINT32 count, FLOAT* newValues, BOOL hasFinished)
    {
        AudioVoice* voice = SaXAudio::Instance.GetVoice((INT32)voiceID);
        if (!voice) return;
        voice->Speed = newValues[0];
        voice->SourceVoice->SetFrequencyRatio(newValues[0]);
        if (hasFinished) voice->m_speedFadeID = 0;
    }

    void AudioVoice::OnFadePanning(INT64 voiceID, UINT32 count, FLOAT* newValues, BOOL hasFinished)
    {
        AudioVoice* voice = SaXAudio::Instance.GetVoice((INT32)voiceID);
        if (!voice) return;
        voice->Panning = newValues[0];
        voice->SetOutputMatrix(newValues[0]);
        if (hasFinished) voice->m_panningFadeID = 0;
    }

    void __stdcall AudioVoice::OnBufferEnd(void* pBufferContext)
    {
        if (m_tempFlush > 0)
        {
            Log(BankID, VoiceID, "[OnBufferEnd] Flush reset");
            m_tempFlush--;
            return;
        }
        Log(BankID, VoiceID, "[OnBufferEnd] Voice finished playing");

        INT32 id = VoiceID;
        thread destroyer([id] ()
        {
            SaXAudio::Instance.RemoveVoice(id);
        });
        destroyer.detach();
    }
}