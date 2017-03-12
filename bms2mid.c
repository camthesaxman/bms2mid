/*
 * Copyright 2017 Cameron Hall
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <assert.h>
#include <stdlib.h>
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#define ARRAY_LENGTH(x) (sizeof(x) / sizeof(*x))

#define MAX_CHANNELS 16  // Midi channels range from 0 to 15, with channel 9 being percussion only
#define STACK_LIMIT 4  // I don't know what the limit is (if any) for nested subroutines

struct MidiTrack
{
    int channel;
    int length;
    uint8_t *buffer;
    int bufferSize;
};

static int voices[8];  // Stores notes that are held simultaneously. The note on/off events have a voice parameter which specifies which of the notes to activate/deactivate
static unsigned long int delay = 0;  // MIDI event delay
static int currTrack = 0;
static bool inTrack = false;  // Set to true when we are processing a track
static unsigned long int savedPos;  // file offset to return to after reading a track
static struct MidiTrack *midiTracks = NULL;
static unsigned int numMidiTracks = 0;
static int metaTrack;
static FILE *midiFile;
static unsigned long int callStack[STACK_LIMIT];
static int callStackTop = 0;
static int *instrList = NULL;
static int instrListCount = 0;
static uint16_t usedChannelMask = 0;
static int ticksPerQNote = 0;

// We will show extremely verbose messages if DEBUG is defined.
#ifdef DEBUG
static void DEBUG_printf(const char *fmt, ...)
{
    va_list args;
    
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    fflush(stdout);
}
#else
static void DEBUG_printf(const char *fmt, ...) {(void)fmt;}
#endif

static void usage(const char *progName)
{
    printf("usage: %s bmsFile midiFile instrumentList\n"
      "where bmsFile is the input .bms file, midiFile is the output .mid file,\n"
      "and instrumentList is a text file containing a list of instrument names\n"
      "or general MIDI numbers for each instrument ID. This file is optional,\n"
      "but the instruments used in the MIDI will probably be wrong without it.\n",
      progName);
}

static void fatal_error(const char *fmt, ...)
{
    va_list args;
    
    fflush(stdout);
    fputs("ERROR! ", stderr);
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    exit(1);
}

//------------------------------------------------------------------------------
// File Read/Write Functions
//------------------------------------------------------------------------------

static uint8_t read_u8(FILE *file)
{
    return fgetc(file);
}

static uint16_t read_u16(FILE *file)
{
    uint16_t val;
    
    val = (fgetc(file) & 0xFF) << 8;
    val |= fgetc(file) & 0xFF;
    return val;
}

static uint32_t read_u24(FILE *file)
{
    uint32_t val;
    
    val = (fgetc(file) & 0xFF) << 16;
    val |= (fgetc(file) & 0xFF) << 8;
    val |= fgetc(file) & 0xFF;
    return val;
}

static uint32_t read_u32(FILE *file)
{
    uint32_t val;
    
    val = (fgetc(file) & 0xFF) << 24;
    val |= (fgetc(file) & 0xFF) << 16;
    val |= (fgetc(file) & 0xFF) << 8;
    val |= fgetc(file) & 0xFF;
    return val;
}

static void write_u16(FILE *file, uint16_t val)
{
    uint8_t buf[2] = {val >> 8, val};
    
    fwrite(buf, 1, sizeof(buf), file);
}

static void write_u32(FILE *file, uint32_t val)
{
    uint8_t buf[4] = {val >> 24, val >> 16, val >> 8, val};
    
    fwrite(buf, 1, sizeof(buf), file);
}

static void fskip(FILE *file, int len)
{
    fseek(file, len, SEEK_CUR);
}

//------------------------------------------------------------------------------
// MIDI Track Functions
//------------------------------------------------------------------------------

static int add_track(void)
{
    int track = numMidiTracks;
    
    numMidiTracks++;
    midiTracks = realloc(midiTracks, numMidiTracks * sizeof(*midiTracks));
    midiTracks[track].length = 0;
    midiTracks[track].buffer = NULL;
    midiTracks[track].channel = -1;
    return track;
}

static void track_write_u8(int track, uint8_t val)
{
    int offset = midiTracks[track].length;
    
    midiTracks[track].length++;
    midiTracks[track].buffer = realloc(midiTracks[track].buffer, midiTracks[track].length);
    midiTracks[track].buffer[offset] = val;
}

static void track_write_u24(int track, uint32_t val)
{
    int offset = midiTracks[track].length;
    
    midiTracks[track].length += 3;
    midiTracks[track].buffer = realloc(midiTracks[track].buffer, midiTracks[track].length);
    midiTracks[track].buffer[offset + 0] = (val >> 16) & 0xFF;
    midiTracks[track].buffer[offset + 1] = (val >> 8) & 0xFF;
    midiTracks[track].buffer[offset + 2] = val & 0xFF;
}

// Encodes val into a variable length quantity, which is used for MIDI event delays
static void track_write_varlen(int track, uint32_t val)
{
    unsigned long int buf = val & 0x7F;
    
    while ((val >>= 7) != 0)
    {
        buf <<= 8;  // move onto next byte
        buf |= (val & 0x7F) | 0x80;  // write 7 bits and set the 8th bit to 1
    }
    while (1)
    {
        track_write_u8(track, buf);
        if (buf & 0x80)
            buf >>= 8;
        else
            break;
    }
}

//------------------------------------------------------------------------------
// BMS Event Handlers
//------------------------------------------------------------------------------

// 0x00 - 0x7F
static void event_note_on(FILE *file, uint8_t pitch)
{
    uint8_t voice = read_u8(file);
    uint8_t volume = read_u8(file);
    
    // simple hack to make the percussion sound reasonably close,
    // though the note numbers do not match up at all with General MIDI drum kits.
    if (midiTracks[currTrack].channel == 9)
        pitch -= 1;
    
    DEBUG_printf("[NOTE_ON]\tpitch %i, voice %i, volume %i\n", pitch, voice, volume);
    assert(voice < 8);
    track_write_varlen(currTrack, delay);
    track_write_u8(currTrack, 0x90 + midiTracks[currTrack].channel);
    track_write_u8(currTrack, pitch);
    track_write_u8(currTrack, volume);
    delay = 0;
    voices[voice] = pitch;
}

// 0x81 - 0x87
static void event_note_off(uint8_t voice)
{
    DEBUG_printf("[NOTE_OFF]\tvoice %i\n", voice);
    assert(voice < 8);
    assert(voices[voice] != -1);
    track_write_varlen(currTrack, delay);
    track_write_u8(currTrack, 0x80 + midiTracks[currTrack].channel);
    track_write_u8(currTrack, voices[voice]);
    track_write_u8(currTrack, 0);
    delay = 0;
    voices[voice] = -1;
}

// 0x80
static void event_delay_u8(FILE *file)
{
    delay += read_u8(file);
    
    DEBUG_printf("[DELAY8]\t%lu\n", delay);
}

// 0x88
static void event_delay_u16(FILE *file)
{
    delay += read_u16(file);
    
    DEBUG_printf("[DELAY16]\t%lu\n", delay);
}

static int get_available_channel(void)
{
    // Search for a channel that hasn't been taken.
    // Avoid using channel 9 because it is percussion only
    for (int i = 0; i < MAX_CHANNELS; i++)
    {
        if ((usedChannelMask & (1 << i)) == 0 && i != 9)
        {
            usedChannelMask |= 1 << i;
            return i;
        }
    }
    // If we have no choice, use channel 9 if it's available
    if ((usedChannelMask & (1 << 9)) == 0)
    {
        usedChannelMask |= 1 << 9;
        return 9;
    }
    fatal_error("Cannot use more than 16 MIDI channels\n");
    return -1;
}

// 0xC1
static void event_track_start(FILE *file)
{
    long int trackOffset;
    
    read_u8(file);
    trackOffset = read_u24(file);
    savedPos = ftell(file);
    fseek(file, trackOffset, SEEK_SET);
    currTrack = add_track();
    midiTracks[currTrack].channel = get_available_channel();
    inTrack = true;
    DEBUG_printf("[TRACK_START]\t%i\n", currTrack);
}

static uint8_t convert_instrument(uint8_t instr)
{
    if (instr < instrListCount)
        return instrList[instr];  // If alternative instrument is specified in list, return that
    else
        return instr;  // Otherwise, don't change it
}

// 0xA4
static void event_instrument(FILE *file)
{
    uint8_t event2 = read_u8(file);
    
    DEBUG_printf("[INSTRUMENT]\t");
    switch (event2)
    {
        case 0x20:  // Bank
        {
            uint8_t bank = read_u8(file);
            
            DEBUG_printf("(set bank) %i\n", bank);
            break;
        }
        case 0x21:  // Instrument
        {
            uint8_t oldInstr = read_u8(file);
            uint8_t instr = convert_instrument(oldInstr);
            
            if (instr == 128)  // Drum Kit - move this track to channel 9
            {
                // Make sure channel 9 is not already in use
                assert((usedChannelMask & (1 << 9)) == 0);
                usedChannelMask &= ~(1 << midiTracks[currTrack].channel);
                usedChannelMask |= (1 << 9);
                midiTracks[currTrack].channel = 9;
                instr = 0;
            }
            track_write_varlen(currTrack, delay);
            track_write_u8(currTrack, 0xC0 + midiTracks[currTrack].channel);
            track_write_u8(currTrack, instr);
            delay = 0;
            DEBUG_printf("(set instrument) %i, %i\n", oldInstr, instr);
            break;
        }
        default:
            // TODO: Figure out what event2 = 7 is supposed to mean
            fskip(file, 1);
            DEBUG_printf("(%u)\n", event2);
    }
}

// 0xFD
static void event_tempo(FILE *file)
{
    uint16_t tempo = read_u16(file);
    
    DEBUG_printf("[TEMPO]\t%u bpm\n", tempo);
    if (inTrack)
        fputs("Warning: setting tempo within a track is not supported\n", stderr);
    else
    {
        unsigned int usec = 60 * 1000000 / tempo;  // microseconds per quarter note
        
        track_write_varlen(metaTrack, delay);
        track_write_u8(metaTrack, 0xFF);
        track_write_u8(metaTrack, 0x51);
        track_write_u8(metaTrack, 0x03);
        track_write_u24(metaTrack, usec);
        delay = 0;
    }
}

// 0xC4
static void event_subroutine_call(FILE *file)
{
    unsigned long int dest = read_u32(file);
    
    if (callStackTop >= STACK_LIMIT)
        fatal_error("Call stack limit reached\n");
    callStack[callStackTop] = ftell(file);  // Push return address onto stack
    callStackTop++;
    fseek(file, dest, SEEK_SET);
    DEBUG_printf("[CALL]\tCall to subroutine 0x%X\n", (unsigned int)dest);
}

// 0xC6
static void event_subroutine_return(FILE *file)
{
    unsigned long int dest;
    
    callStackTop--;
    if (callStackTop < 0)
        fatal_error("Attempted to return outside of subroutine\n");
    dest = callStack[callStackTop];  // Pop return address from stack
    fseek(file, dest, SEEK_SET);
    DEBUG_printf("[RETURN]\tReturning to 0x%X\n", (unsigned int)dest);
}

// 0xFE
static void event_ticks_per_qnote(FILE *file)
{
    uint16_t val = read_u16(file);
    
    DEBUG_printf("[TICKS]\t");
    if (ticksPerQNote != 0)
        DEBUG_printf("Warining: Ticks per quarter note already set. Ignoring.\n");
    else
    {
        DEBUG_printf("Setting ticks per quarter note to %u\n", val);
        ticksPerQNote = val;
    }
}

// 0x9C
static void event_volume(FILE *file)
{
    uint8_t event2 = read_u8(file);
    
    DEBUG_printf("[VOLUME]\t");
    switch (event2)
    {
    case 0:  // Volume change
    {
        uint8_t volume = read_u8(file);
        uint8_t duration = read_u8(file);  // Not really sure what this is for.
        
        assert(volume <= 127);
        DEBUG_printf("(set volume) vol = %u, duration = %u\n", volume, duration);
        track_write_varlen(currTrack, delay);
        track_write_u8(currTrack, 0xB0 + midiTracks[currTrack].channel);
        track_write_u8(currTrack, 0x07);
        track_write_u8(currTrack, volume);
        delay = 0;
        break;
    }
    case 0x09:  // Vibrato intensity?
        DEBUG_printf("(vibrato?)\n");
        fskip(file, 2);
        break;
    default:
        DEBUG_printf("(unknown)\n");
        fskip(file, 2);
    }
}

// 0x9A
static void event_pan(FILE *file)
{
    uint8_t event2 = read_u8(file);
    
    DEBUG_printf("[PAN]\t");
    switch (event2)
    {
        case 0x03:  // Change panning
        {
            uint8_t pan = read_u8(file);
            uint8_t duration = read_u8(file);
            
            assert(pan <= 127);
            DEBUG_printf("(set pan) pan = %u, duration = %u\n", pan, duration);
            track_write_varlen(currTrack, delay);
            track_write_u8(currTrack, 0xB0 + midiTracks[currTrack].channel);
            track_write_u8(currTrack, 0x0A);
            track_write_u8(currTrack, pan);
            delay = 0;
            break;
        }
        default:
            DEBUG_printf("(unknown)\n");
            fskip(file, 2);
    }
}

// We don't know what this event does. Just print out its data
static void event_unknown(FILE *file, uint8_t event, int length)
{
    unsigned long int addr = ftell(file) - 1;
    
    DEBUG_printf("[UNKNOWN 0x%X]\t", event);
    for (int i = 0; i < length; i++)
    {
        uint8_t val = read_u8(file);
        DEBUG_printf("0x%X ", val);
    }
    DEBUG_printf(" at address 0x%X\n", (unsigned int)addr);
}

static void read_bms(FILE *file)
{
    metaTrack = add_track();
    
    while (1)
    {
        uint8_t event = read_u8(file);
        
        switch (event)
        {
        case 0x80:
            event_delay_u8(file);
            break;
        case 0x88:
            event_delay_u16(file);
            break;
        case 0xC1:
            event_track_start(file);
            break;
        case 0x9A:
            event_pan(file);
            break;
        case 0x9C:
            event_volume(file);
            break;
        case 0xA4:
            event_instrument(file);
            break;
        case 0x9E:  // Pitch bend, probably
            event_unknown(file, event, 2);
            break;
        
        // These events appear in mboss.bms and enemy2.bms. I have no idea what they do.
        case 0xCC:
            event_unknown(file, event, 2);
            break;
        case 0xAC:  // seems to always be followed by a 0xCC event.
        {
            uint8_t val1 = read_u8(file);
            uint8_t val2 = read_u8(file);
            uint8_t val3 = read_u8(file);
            
            DEBUG_printf("[UNKNOWN 0xAC] 0x%X, 0x%X, 0x%X\n", val1, val2, val3);
            if (val3 == 0)
                goto track_end;
            break;
        }
        case 0xAD:
            event_unknown(file, event, 3);
            break;
        case 0xD6:
            event_unknown(file, event, 1);
            break;
        
        case 0xF4:
            event_unknown(file, event, 1);
            break;
        case 0x98:  // seems to appear near the beginning of a track
        case 0xE6:  // seems to appear near the beginning of a track
        case 0xE7:
            event_unknown(file, event, 2);
            break;
        case 0xCB:  // Not really sure how long this is, but 7 bytes seems to do the trick.
            event_unknown(file, event, 7);
            break;
        case 0xC4:
            event_subroutine_call(file);
            break;
        case 0xC6:
            event_subroutine_return(file);
            break;
        case 0xC8:  // Goto event for looping. We ignore this because MIDIs can't loop
        {
            uint8_t val1 = read_u8(file);
            uint8_t val2 = read_u8(file);
            uint8_t val3 = read_u8(file);
            uint8_t val4 = read_u8(file);
            
            DEBUG_printf("[GOTO] %u, %u, %u, %u\n", val1, val2, val3, val4);
            break;
        }
        case 0xFD:
            event_tempo(file);
            break;
        case 0xFE:
            event_ticks_per_qnote(file);
            break;
        case 0xFF:  // End of track
            DEBUG_printf("[TRACK_END]\t%i\n", currTrack);
          track_end:
            track_write_varlen(currTrack, 0);
            track_write_u8(currTrack, 0xFF);
            track_write_u8(currTrack, 0x2F);
            track_write_u8(currTrack, 0);
            if (!inTrack)
            {
                // End of meta track
                track_write_varlen(metaTrack, 0);
                track_write_u8(metaTrack, 0xFF);
                track_write_u8(metaTrack, 0x2F);
                track_write_u8(metaTrack, 0);
                return;
            }
            fseek(file, savedPos, SEEK_SET);
            delay = 0;
            inTrack = false;
            break;
        default:
            if (event < 0x80)  // Note on
                event_note_on(file, event);
            else if (event >= 0x81 && event <= 0x87)  // Note off
                event_note_off(event & 7);
            else
            {
                fatal_error("Unhandled BMS event 0x%X at address 0x%X\n",
                  event, (unsigned int)(ftell(file) - 1));
            }
        }
    }
}

static void create_instrument_conversion_table(FILE *file)
{
    const char *const instrNames[] =
    {
        // Piano
        "Acoustic Grand Piano", "Bright Piano", "Electric Grand Piano", "Honky-tonk Piano", "Electric Piano 1", "Electric Piano 2", "Harpsichord", "Clavinet",
        // Melodic Percussion
        "Celesta", "Glockenspiel", "Music Box", "Vibraphone", "Marimba", "Xylophone", "Tubular Bells", "Dulcimer",
        // Organ
        "Hammond Organ", "Percussive Organ", "Rock Organ", "Church Organ", "Reed Organ", "Accordian", "Harmonica", "Tango Accordian",
        // Guitar
        "Nylon String Guitar", "Steel String Guitar", "Jazz Guitar", "Clean Electric Guitar", "Muted Guitar", "Overdrive Guitar", "Distortion Guitar", "Guitar Harmonics",
        // Bass
        "Acoustic Bass", "Fingered Bass", "Picked Bass", "Fretless Bass", "Slap Bass 1", "Slap Bass 2", "Synth Bass 1", "Synth Bass 2",
        // String
        "Violin", "Viola", "Cello", "Contrabass", "Tremolo Strings", "Pizzicato Strings", "Orchestral Harp", "Timpani",
        // Ensemble
        "String Ensemble 1", "String Ensemble 2", "Synth Strings 1", "Synth Strings 2", "Choir Ahh", "Choir Oohh", "Synth Voice", "Orchestral Hit",
        // Brass
        "Trumpet", "Trombone", "Tuba", "Muted Trumpet", "French Horn", "Brass Section", "Synth Brass 1", "Synth Brass 2",
        // Reed
        "Soprano Sax", "Alto Sax", "Tenor Sax", "Baritone Sax", "Oboe", "English Horn", "Bassoon", "Clarinet",
        // Pipe
        "Piccolo", "Flute", "Recorder", "Pan Flute", "Blown Bottle", "Shakuhachi", "Whistle", "Ocarina",
        // Synth Lead
        "Square Lead", "Sawtooth Lead", "Calliope Lead", "Chiff Lead", "Charang Lead", "Voice Lead", "Fifth Lead", "Bass & Lead",
        // Synth Pad
        "New Age", "Warm", "Polysynth", "Choir", "Bowed", "Metallic", "Halo", "Sweep",
        // Synth FX
        "FX Rain", "FX Soundtrack", "FX Crystal", "FX Atmosphere", "FX Brightness", "FX Goblins", "FX Echo Drops", "FX Star Theme",
        // Ethnic
        "Sitar", "Banjo", "Shamisen", "Koto", "Kalimba", "Bagpipe", "Fiddle", "Shanai",
        // Percussive
        "Tinkle Bell", "Agogo", "Steel Drums", "Woodblock", "Taiko Drum", "Melodic Tom", "Synth Drum", "Reverse Cymbal",
        // Sound Effects
        "Guitar Fret Noise", "Breath Noise", "Seashore", "Bird Tweet", "Telephone Ring", "Helicopter", "Applause", "Gunshot",
        "Drum Kit",
    };
    size_t bufferSize = 1;
    char *buffer = malloc(bufferSize);
    bool endOfFile = false;
    
    while (!endOfFile)
    {
        int offset = 0;
        int instrNum;
        
        buffer[0] = '\0';        
        while (1)
        {
            if (fgets(buffer + offset, bufferSize - offset, file) == NULL)
                goto done;
            offset = strlen(buffer);
            if (offset + 1 < bufferSize)
                break;
            bufferSize *= 2;
            buffer = realloc(buffer, bufferSize);
        }
        if (offset == 0)
            continue;
        char *nl = strchr(buffer, '\n');
        if (nl != NULL)
            *nl = '\0';
        if (sscanf(buffer, "%i", &instrNum) != 1)  // If it's not a number, check the names.
        {
            char *name;
            
            // Strip space
            name = buffer + offset - 1;
            while (isspace(*name))
                *(name--) = '\0';
            name = buffer;
            while (isspace(*name))
                name++;
            
            for (instrNum = 0; instrNum < ARRAY_LENGTH(instrNames); instrNum++)
            {
                if (strcmp(name, instrNames[instrNum]) == 0)
                    goto got_instrument;
            }
            fatal_error("Unknown instrument '%s'\n", name);
        }
      got_instrument:
        DEBUG_printf("Instrument %i is %s\n", instrListCount, instrNames[instrNum]);
        instrList = realloc(instrList, (instrListCount + 1) * sizeof(*instrList));
        instrList[instrListCount] = instrNum;
        instrListCount++;
    }
  done:
    free(buffer);
}

int main(int argc, char **argv)
{
    FILE *bmsFile;
    
    // MinGW's stupid assert function aborts without flushing stderr, so we never get to see the message.
    // We can work around that by disabling buffering on stderr.
#if defined(_WIN32) && !defined(NDEBUG)
    setvbuf(stderr, NULL, _IONBF, 0);
#endif

    if (argc != 3 && argc != 4)
    {
        usage(argv[0]);
        return 1;
    }
    
    // Open bms file
    bmsFile = fopen(argv[1], "rb");
    if (bmsFile == NULL)
        fatal_error("failed to open input file '%s': %s\n", argv[1], strerror(errno));
    
    // Open midi file
    midiFile = fopen(argv[2], "wb");
    if (midiFile == NULL)
        fatal_error("failed to open output file '%s': %s\n", argv[2], strerror(errno));
    
    if (argc == 4)
    {
        FILE *convTblFile = fopen(argv[3], "r");
        
        if (convTblFile == NULL)
            fatal_error("failed to open instrument conversion file '%s': %s\n", argv[3], strerror(errno));
        create_instrument_conversion_table(convTblFile);
        fclose(convTblFile);
    }
    
    read_bms(bmsFile);
    
    // Now, actually write the MIDI file
    
    // Write header chunk
    fputs("MThd", midiFile);             // chunk type
    write_u32(midiFile, 6);              // chunk length
    write_u16(midiFile, 1);              // format type
    write_u16(midiFile, numMidiTracks);  // number of tracks
    write_u16(midiFile, (ticksPerQNote != 0) ? ticksPerQNote : 120);  // ticks per quarter note (default to 120 if not set)
    
    // Write tracks
    for (unsigned int i = 0; i < numMidiTracks; i++)
    {
        DEBUG_printf("Track %u: channel %i\n", i, midiTracks[i].channel);
        fputs("MTrk", midiFile);
        write_u32(midiFile, midiTracks[i].length);
        fwrite(midiTracks[i].buffer, 1, midiTracks[i].length, midiFile);
    }
    DEBUG_printf("%i midi tracks\n", numMidiTracks);
    fclose(bmsFile);
    fclose(midiFile);
    return 0;
}
