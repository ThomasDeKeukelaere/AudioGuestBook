// LIBRARIES
    #include <Arduino.h>
    #include <Audio.h>
    #include <Bounce2.h>
    #include <MTP_Teensy.h>
    #include <SD.h>
    #include <SPI.h>
    #include <Wire.h>
    #include <play_sd_wav.h>

// CONSTANT VARIABLES
    const int HandsetButtonPin = 0;
    const int PushButtonPin = 1;
    const int MosiPin = 7;
    const int SckPin = 14;
    const int CsPin = 10;  // Teensy 4.1 onboard slot: BUILTIN_SDCARD - Audioadaptor slot: 10

    const int BitsPerSample = 16;
    const int SamplesPerBlock = 128;
    const int BytesPerBlock = 256;
    const int BlocksPerWrite = 16;

// GLOBAL VARIABLES
    Button HandsetButton = Button();
    Button PushButton = Button();
    const bool HandsetButtonPressedState = LOW;  // If handset down = contact closed -> LOW (otherwise HIGH) (Raven: HIGH)
    const bool PushButtonPressedState = LOW;     // If pushbutton pressed = contact closed -> LOW (otherwise HIGH)

    unsigned long SizeRecording = 0L;
    bool MTPActive = false;
    bool RecordingToShort = false;
    bool ConfigMode = false;
    elapsedMillis Timer = 0;

// VOLUME VARIABLES
    float VolumeAudioShield = 1.0f;    // Overall volume of the AudioShield   (Bluebird: 1.0f,   Raven: 1.0f,   Flamingo: 1.0f,)
    float GainMic = 8.0f;              // Amplify Microphone                  (Bluebird: 20.0f,  Raven: 7.0f,   Flamingo: 8.0f,)
    float GainMixOscillator = 1.0f;    // Amplify Oscillator (Beep)           (Bluebird: 1.0f,   Raven: 1.0f,   Flamingo: 1.0f,)
    float GainMixInstructions = 0.6f;  // Amplify Playback Audio Instructions (Bluebird: 0.4f,   Raven: 0.6f,   Flamingo: 0.6f,)
    float GainMixRecordings = 1.0f;    // Amplify Playback Audio Recordings   (Bluebird: 0.1f,   Raven: 1.0f,   Flamingo: 1.0f,)
    float VolumeBeepLoud = 1.0f;       // Volume Beep when handset down       (Bluebird: 1.0f,   Raven: 1.0f,   Flamingo: 1.0f,)
    float VolumeBeepQuiet = 0.1f;      // Volume Beep when handset at ear     (Bluebird: 0.1f,   Raven: 0.1f,   Flamingo: 0.1f,)

// FILE VARIABLES
    File RecFile;
    uint64_t MinFileSize = 150000;                                                          // Minimum filesize in bytes (1s ~ 88200 bytes)
    uint64_t MaxRecTime = 360000;                                                           // Maximum recording time in ms (6min = 360.000)                                                     // Minimum file size in bytes to be saved (1s ~ 88.2kB)
    char FileNameLastRec[] = "Don't Delete/No new message.wav";                             // Filename Last saved recording
    const char FilenameTemp[] = "temp.wav";                                                 // Temporary filename of message being recorded
    const char FileNameNewGreeting[] = "Greeting.wav";                                      // Filename new greeting
    const char FileNameDefaultGreeting[] = "Don't Delete/Standard greeting.wav";            // Filename standard greeting
    const char FileNameChangeGreeting[] = "Don't Delete/Change greeting instructions.wav";  // Filename change greeting instructions
    const char FileNameConfirmGreeting[] = "Don't Delete/Confirm new greeting.wav";         // Filename message confirm to change greeting
    const char FileNameGreetingSaved[] = "Don't Delete/Greeting saved.wav";                 // Filename message greeting saved
    const char FileNameMessageTooShort[] = "Don't Delete/Message too short.wav";            // Filename message file to short error

// AUDIO ELEMENTS & SETUP  (import code into -> https://www.pjrc.com/teensy/gui/)
    AudioSynthWaveform Oscillator;
    AudioPlaySdWav Instructions;
    AudioPlaySdWav Recordings;
    AudioMixer4 Mixer;
    AudioInputI2S Microphone;
    AudioRecordQueue Queue;
    AudioOutputI2S Speaker;
    AudioConnection patchCord1(Oscillator, 0, Mixer, 0);
    AudioConnection patchCord2(Instructions, 0, Mixer, 1);
    AudioConnection patchCord3(Recordings, 0, Mixer, 2);
    AudioConnection patchCord4(Microphone, 0, Queue, 0);
    AudioConnection patchCord5(Mixer, 0, Speaker, 0);
    AudioConnection patchCord6(Mixer, 0, Speaker, 1);
    AudioControlSGTL5000 sgtl5000_1;

// DEVICE STATES
enum States {
    Init,
    Ready,
    Greeting,
    Recording,
    Playback,
    ChangeGreeting
};
States CurrentState = Init;

// FUNCTIONS DECLARATIONS
    void PlayBeep(float BeepVolume, int Counts, unsigned int IntervalTime_ms);
    void SetCurrentStateTo(States NewState);
    void StartRecording();
    void ContinueRecording();
    void StopRecording();
    void WriteToFile(File &file, int Value, int Size);
    void WriteFileHeader();

// PROGRAM INIT
void setup() {
    // Init Teensy pins
    HandsetButton.attach(HandsetButtonPin, INPUT_PULLUP);
    HandsetButton.interval(40);
    HandsetButton.setPressedState(HandsetButtonPressedState);
    PushButton.attach(PushButtonPin, INPUT_PULLUP);
    PushButton.interval(40);
    PushButton.setPressedState(PushButtonPressedState);

    // Audio setup & volumes
    AudioMemory(60);
    sgtl5000_1.enable();
    sgtl5000_1.volume(VolumeAudioShield);
    sgtl5000_1.inputSelect(AUDIO_INPUT_MIC);
    sgtl5000_1.micGain(GainMic);
    Mixer.gain(0, GainMixOscillator);
    Mixer.gain(1, GainMixInstructions);
    Mixer.gain(2, GainMixRecordings);

    // Init SD card
    SPI.setMOSI(MosiPin);
    SPI.setSCK(SckPin);
    SD.begin(CsPin);
    while (!SD.begin(CsPin)) {
        PlayBeep(VolumeBeepLoud, 4, 250);
        delay(2000);
    }

    // Init Media transfer protocol
    MTP.begin();
    MTP.addFilesystem(SD, "Blue Bird Stories");

    // Wait 1s to read pushbutton
    Timer = 0;
    while (Timer <= 1000) {
        PushButton.update();
    }

    // Pushbutton pressed: activate ConfigMode, play 3 beeps
    if (PushButton.isPressed()) {
        ConfigMode = true;
        PlayBeep(VolumeBeepLoud, 3, 250);
    } else {
        ConfigMode = false;
        PlayBeep(VolumeBeepLoud, 1, 1000);
    }

    // Init Ready mode
    SetCurrentStateTo(Ready);
}

// PROGRAM MAIN
void loop() {
    switch (CurrentState) {
        case Init:
            break;

        case Ready:
            // Read buttons
            HandsetButton.update();
            PushButton.update();

            // Go to "Greeting" when handset is lifted
            if (HandsetButton.released()) {
                SetCurrentStateTo(Greeting);
                MTPActive = false;
            }
            // CONFIG MODE: Activate Media Transfer if pushbutton is pressed 5s
            if ((ConfigMode && PushButton.isPressed()) && PushButton.duration() > 5000) {
                MTPActive = true;
            }
            break;

        case Greeting:
            // Wait 1s
            Timer = 0;
            while (Timer <= 1000 && !PushButton.isPressed()) {
                PushButton.update();
            }

            // Play Greeting
            if (ConfigMode) {
                Instructions.play(FileNameChangeGreeting);
            } else if (SD.exists(FileNameNewGreeting)) {
                Recordings.play(FileNameNewGreeting);
            } else {
                Instructions.play(FileNameDefaultGreeting);
            }

            while (true) {
                HandsetButton.update();
                PushButton.update();

                // Handset down: Stop greeting or instruction & go to "Ready"
                if (HandsetButton.isPressed()) {
                    Instructions.stop();
                    Recordings.stop();
                    SetCurrentStateTo(Ready);
                    break;
                }

                // Pushbutton pressed: Stop greeting or instruction & go to "Playback"
                if (PushButton.isPressed()) {
                    Instructions.stop();
                    Recordings.stop();
                    PlayBeep(VolumeBeepQuiet, 1, 250);
                    SetCurrentStateTo(Playback);
                    break;
                }

                // Greeting or instruction done: play beep & go to "Recording"
                if (Instructions.isStopped() && Recordings.isStopped()) {
                    PlayBeep(VolumeBeepQuiet, 1, 500);
                    SetCurrentStateTo(Recording);
                    break;
                }
            }
            break;

        case Recording:
            // Start recording
            StartRecording();
            Timer = 0;

            while (true) {
                // Read buttons
                HandsetButton.update();
                PushButton.update();

                // Continue recording
                ContinueRecording();

                // Maximum recording time
                if (Timer > MaxRecTime) {
                    StopRecording();
                    PlayBeep(VolumeBeepQuiet, 3, 250);
                    SetCurrentStateTo(Ready);
                    break;
                }

                // CONFIG MODE & Pushbutton pressed: stop recording & check file size
                if (ConfigMode && (PushButton.isPressed() || HandsetButton.isPressed())) {
                    StopRecording();

                    // Recording to short OR Handset down: Remove rec, set default last rec, play warning & go to "Ready"
                    if (RecordingToShort || HandsetButton.isPressed()) {
                        SD.remove(FileNameLastRec);
                        memcpy(FileNameLastRec, "Don't Delete/No new message.wav", sizeof("Don't Delete/No new message.wav"));
                        Instructions.play(FileNameMessageTooShort);
                        while (!Instructions.isStopped() && !HandsetButton.isPressed()) {
                            HandsetButton.update();
                        }
                        Instructions.stop();
                        SetCurrentStateTo(Ready);
                    }
                    // Recording not to short: play beep & go to "ChangeGreeting"
                    else {
                        PlayBeep(VolumeBeepQuiet, 1, 250);
                        SetCurrentStateTo(ChangeGreeting);
                    }
                    break;
                }

                // Handset down: Stop recording & check file size
                if (HandsetButton.isPressed()) {
                    StopRecording();

                    // Recording to short: Remove file, set default last rec & go to "Ready"
                    if (RecordingToShort) {
                        SD.remove(FileNameLastRec);
                        memcpy(FileNameLastRec, "Don't Delete/No new message.wav", sizeof("Don't Delete/No new message.wav"));
                        SetCurrentStateTo(Ready);
                    }
                    // Recording not to short: play 3 beeps & go to "Ready"
                    else {
                        PlayBeep(VolumeBeepQuiet, 3, 250);
                        SetCurrentStateTo(Ready);
                    }
                    break;
                }
            }
            break;

        case Playback:
            // Play last recording OR Current greeting in CONFIGMODE
            if (ConfigMode && SD.exists(FileNameNewGreeting)) {
                Recordings.play(FileNameNewGreeting);
            } else if (ConfigMode) {
                Instructions.play(FileNameDefaultGreeting);
            } else {
                Recordings.play(FileNameLastRec);
            }

            while (true) {
                // Read buttons
                HandsetButton.update();

                // Handset down: Stop & go to "Ready"
                if (HandsetButton.isPressed()) {
                    Recordings.stop();
                    Instructions.stop();
                    SetCurrentStateTo(Ready);
                    break;
                }
                // Message stopped: play 3 beeps & go to "Ready"
                if (Instructions.isStopped() && Recordings.isStopped()) {
                    PlayBeep(VolumeBeepQuiet, 3, 250);
                    SetCurrentStateTo(Ready);
                    break;
                }
            }
            break;

        case ChangeGreeting:
            // Replay recording
            Recordings.play(FileNameLastRec);
            while (!Recordings.isStopped()) {
                // Handset down: stop playback & go to "Ready"
                HandsetButton.update();
                if (HandsetButton.isPressed()) {
                    Recordings.stop();
                    SetCurrentStateTo(Ready);
                    break;
                }
            }

            if (CurrentState == ChangeGreeting) {
                // Play beep & Wait
                PlayBeep(VolumeBeepQuiet, 2, 250);
                delay(500);

                // Play Confirmation message & Reset timer
                Instructions.play(FileNameConfirmGreeting);
                Timer = 0;

                while (true) {
                    // Read buttons
                    HandsetButton.update();
                    PushButton.update();

                    // Message stopped: Start timer
                    if (!Instructions.isStopped()) {
                        Timer = 0;
                    }

                    // Handset down OR Time over: Stop message, play beep, go to "Ready"
                    if (HandsetButton.isPressed() || Timer > 6000) {
                        Instructions.stop();
                        PlayBeep(VolumeBeepQuiet, 1, 1000);
                        SetCurrentStateTo(Ready);
                        break;
                    }

                    // Pushbutton Pressed:
                    if (PushButton.isPressed()) {
                        // Stop message
                        Instructions.stop();

                        // Delete old greeting
                        if (SD.exists(FileNameNewGreeting)) {
                            SD.remove(FileNameNewGreeting);
                        }
                        // Rename the recording to "Greeting.wav"
                        SD.rename(FileNameLastRec, FileNameNewGreeting);

                        // Play 3 beeps & play "greeting is saved" message
                        PlayBeep(VolumeBeepQuiet, 3, 250);
                        Instructions.play(FileNameGreetingSaved);
                        while (!Instructions.isStopped()) {
                            // Handset down: stop message
                            HandsetButton.update();
                            if (HandsetButton.isPressed()) {
                                Instructions.stop();
                                break;
                            }
                        }
                        // Go to "Ready" state
                        SetCurrentStateTo(Ready);
                        break;
                    }
                }
                break;
            }
    }

    // Run Media transfer if activated
    if (MTPActive) {
        MTP.loop();
    }
}

// function Beep(volume, amount off beeps, duration of 1 beep in ms)
void PlayBeep(float BeepVolume, int Counts, unsigned int IntervalTime_ms) {
    for (int i = 0; i < Counts; i++) {
        // start beep
        Oscillator.begin(BeepVolume, 440, WAVEFORM_SINE);
        delay(IntervalTime_ms);
        // stop beep
        Oscillator.amplitude(0);
        // skip delay for last beep
        if (i + 1 < Counts) {
            delay(IntervalTime_ms);
        }
    }
    return;
}

// function SetCurrentStateTo(New State)
void SetCurrentStateTo(States NewState) {
    // Extra switch conditions for Ready state: buttons must be in default state
    if (NewState == Ready) {
        while (!HandsetButton.isPressed() || PushButton.isPressed()) {
            HandsetButton.update();
            PushButton.update();
        }
    }
    // Extra switch conditions for ChangeGreeting State: pushbutton must be released
    if (NewState == ChangeGreeting) {
        while (PushButton.isPressed()) {
            PushButton.update();
        }
    }
    CurrentState = NewState;
};

// function StartRecording
void StartRecording() {
    // Open new file & start recording to Queue
    delay(50);  // To remove clip
    RecFile = SD.open(FilenameTemp, FILE_WRITE);
    Queue.begin();
}

// function ContinueRecording
void ContinueRecording() {
    if (Queue.available() >= BlocksPerWrite) {
        byte buffer[BlocksPerWrite * BytesPerBlock];
        for (int i = 0; i < BlocksPerWrite; i++) {
            memcpy(buffer + i * BytesPerBlock, Queue.readBuffer(), BytesPerBlock);
            Queue.freeBuffer();
        }
        RecFile.write(buffer, sizeof buffer);
        SizeRecording += sizeof buffer;
    }
}

// function StopRecording
void StopRecording() {
    // Stop recording
    Queue.end();

    // Write remaining samples
    while (Queue.available() > 0) {
        RecFile.write((byte *)Queue.readBuffer(), BytesPerBlock);
        Queue.freeBuffer();
        SizeRecording += BytesPerBlock;
    }
    // Write out header
    WriteFileHeader();

    // Check file size
    if (RecFile.size() < MinFileSize) {
        RecFile.close();
        RecordingToShort = true;
    } else {
        RecFile.close();
        RecordingToShort = false;
    }

    // Search first available filename & rename temp.wav
    for (uint16_t i = 0; i < 9999; i++) {
        snprintf(FileNameLastRec, 11, "%05d.wav", i);
        if (!SD.exists(FileNameLastRec)) {
            break;
        }
    }
    SD.rename(FilenameTemp, FileNameLastRec);
}

// function WriteToFile(File to write, Value, Size of Value in bytes)
void WriteToFile(File &file, int Value, int Size) {
    file.write(reinterpret_cast<char *>(&Value), Size);
}

// function WriteFileHeader //http://soundfile.sapp.org/doc/WaveFormat/
void WriteFileHeader() {
    // Search begin (It will overwrite the first 44bytes of audio with the header)
    RecFile.seek(0);

    // Write Header Chunk
    RecFile.write("RIFF");                       // ChunkID
    WriteToFile(RecFile, SizeRecording - 8, 4);  // ChunkSize (size of the rest of the chunk following this number)
    RecFile.write("WAVE");                       // Format

    // Write Format Chunk
    RecFile.write("fmt ");           // SubchunkID
    WriteToFile(RecFile, 16, 4);     // SubchunkSize = 16
    WriteToFile(RecFile, 1, 2);      // AudioFormat = 1: PulseCodeModulation
    WriteToFile(RecFile, 1, 2);      // NumChannels = 1: Mono
    WriteToFile(RecFile, 44100, 4);  // SampleRate = 44.1kHz
    WriteToFile(RecFile, 88200, 4);  // ByteRate = SampleRate * NumChannels * (BitsPerSample / 8)
    WriteToFile(RecFile, 2, 2);      // BlockAlign = NumChannels * (BitsPerSample / 8)
    WriteToFile(RecFile, 16, 2);     // BitsPerSample = 16

    // Write Data Chunk
    RecFile.write("data");                        // Subchunk2ID
    WriteToFile(RecFile, SizeRecording - 44, 4);  // Subchunk2Size
}