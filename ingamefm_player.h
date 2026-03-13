#pragma once
#include <sstream>
// =============================================================================
// ingamefm_player.h  — part of ingamefm (header-only)
//
// Parses Furnace tracker pattern text and drives a IngameFMChip instance
// via SDL audio callback at the correct tempo.
//
// Furnace pattern text format (as copied from the tracker):
//   org.tildearrow.furnace - Pattern Data (N)
//   <rows count>
//   <row> | <row> | ...
//   ...
//
// Each channel column is exactly (3 + 2 + 2 + effects*3) characters wide,
// padded with dots.  We ignore effects entirely.
// Note field  : 3 chars  — e.g. "C-4", "F#2", "OFF", "..."
// Instrument  : 2 hex    — e.g. "0B", ".."
// Volume      : 2 hex    — e.g. "74", ".."
// Effects pair: 3 chars  — e.g. "EC0", "..." (ignored)
//
// Usage:
//   IngameFMPlayer player;
//   player.set_song(song_text, tick_rate, speed);
//   player.add_patch(0, PATCH_SLAP_BASS);
//   player.play(sdl_audio_device);   // blocks until song ends (or call update() in your loop)
// =============================================================================

#include "ingamefm_patchlib.h"

#include <SDL.h>
#include <string>
#include <vector>
#include <array>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <algorithm>
#include <atomic>
#include <mutex>

// =============================================================================
// Internal note representation
// =============================================================================

static constexpr int NOTE_NONE = -1;   // "..." — carry over
static constexpr int NOTE_OFF  = -2;   // "OFF"

struct IngameFMEvent
{
    int note;       // MIDI note number, NOTE_NONE, or NOTE_OFF
    int instrument; // 0-255, or -1 = inherit
    int volume;     // 0-127 (TL), or -1 = inherit
};

struct IngameFMRow
{
    std::vector<IngameFMEvent> channels;
};

struct IngameFMSong
{
    int num_rows    = 0;
    int num_channels= 0;
    std::vector<IngameFMRow> rows;
};

// =============================================================================
// Parse helpers
// =============================================================================

// Trim trailing whitespace / newlines from a string
static std::string trim_right(const std::string& s)
{
    size_t end = s.find_last_not_of(" \t\r\n");
    return (end == std::string::npos) ? "" : s.substr(0, end + 1);
}

// Parse a single note name string (3 chars) into a MIDI note or special value.
// Returns NOTE_OFF, NOTE_NONE, or a MIDI note number [0..127].
// Throws std::runtime_error on unrecognised input.
static int parse_note_field(const char* nc, int line_num)
{
    // All dots = no event
    if (nc[0] == '.' && nc[1] == '.' && nc[2] == '.')
        return NOTE_NONE;

    // OFF
    if (nc[0] == 'O' && nc[1] == 'F' && nc[2] == 'F')
        return NOTE_OFF;

    // Note name (letter, sharp-or-dash, octave digit)
    // Letter
    int semitone = -1;
    switch (nc[0])
    {
        case 'C': semitone = 0;  break;
        case 'D': semitone = 2;  break;
        case 'E': semitone = 4;  break;
        case 'F': semitone = 5;  break;
        case 'G': semitone = 7;  break;
        case 'A': semitone = 9;  break;
        case 'B': semitone = 11; break;
        default:
            throw std::runtime_error(
                std::string("Line ") + std::to_string(line_num) +
                ": expected note letter (C D E F G A B), OFF, or '...' but got '" +
                std::string(nc, 3) + "'");
    }

    // Sharp?
    if (nc[1] == '#')      semitone++;
    else if (nc[1] == '-') { /* natural */ }
    else
        throw std::runtime_error(
            std::string("Line ") + std::to_string(line_num) +
            ": expected '#' or '-' for accidental in note '" + std::string(nc, 3) + "'");

    // Octave digit
    if (nc[2] < '0' || nc[2] > '9')
        throw std::runtime_error(
            std::string("Line ") + std::to_string(line_num) +
            ": expected octave digit 0-9 in note '" + std::string(nc, 3) + "'");

    int octave   = nc[2] - '0';
    int midiNote = 12 + octave * 12 + semitone;  // C0=12
    return midiNote;
}

// Parse two hex chars, return value or -1 if ".."
static int parse_hex2(const char* p, int line_num, const char* field_name)
{
    if (p[0] == '.' && p[1] == '.')
        return -1;

    // Both must be hex digits
    auto hex = [&](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        throw std::runtime_error(
            std::string("Line ") + std::to_string(line_num) +
            ": expected hex digit for " + field_name + " but got '" + c + "'");
        return 0;
    };
    return hex(p[0]) * 16 + hex(p[1]);
}

// =============================================================================
// Parse a full channel column from a row string.
// colStart: character index into 'row' where this channel begins.
// colWidth: total character width of the channel (note 3 + inst 2 + vol 2 + effects).
// =============================================================================

static IngameFMEvent parse_channel_column(
    const std::string& row,
    size_t colStart,
    size_t colWidth,
    int line_num)
{
    IngameFMEvent ev;
    ev.note       = NOTE_NONE;
    ev.instrument = -1;
    ev.volume     = -1;

    // Pad row to ensure we can read colWidth chars
    std::string col = row.substr(colStart, colWidth);
    while (col.size() < colWidth)
        col += '.';

    // Note (3 chars)
    ev.note = parse_note_field(col.c_str(), line_num);

    // Instrument (2 hex chars at offset 3)
    if (col.size() >= 5)
        ev.instrument = parse_hex2(col.c_str() + 3, line_num, "instrument");

    // Volume (2 hex chars at offset 5)
    if (col.size() >= 7)
        ev.volume = parse_hex2(col.c_str() + 5, line_num, "volume");

    // Effects at offset 7+ are silently ignored.

    return ev;
}

// =============================================================================
// Song parser
// =============================================================================

// Parse the furnace pattern text into a IngameFMSong.
// Throws std::runtime_error with a descriptive message on any parse error.
static IngameFMSong parse_ingamefm_song(const std::string& text)
{
    // Split into lines
    std::vector<std::string> lines;
    {
        std::istringstream ss(text);
        std::string line;
        while (std::getline(ss, line))
            lines.push_back(trim_right(line));
    }

    // Remove leading blank lines
    while (!lines.empty() && lines.front().empty())
        lines.erase(lines.begin());

    if (lines.empty())
        throw std::runtime_error("Song text is empty");

    int lineNum = 1;

    // Line 1: optional header "org.tildearrow.furnace - Pattern Data (N)"
    // If present, skip it.
    if (lines[0].find("org.tildearrow.furnace") != std::string::npos)
    {
        lines.erase(lines.begin());
        lineNum++;
    }

    // Remove leading blank lines again (after header)
    while (!lines.empty() && lines.front().empty())
    {
        lines.erase(lines.begin());
        lineNum++;
    }

    if (lines.empty())
        throw std::runtime_error("Song text has no content after header");

    // Next non-blank line: row count (a single integer)
    int num_rows = 0;
    {
        const std::string& rowCountLine = lines[0];
        char* endp = nullptr;
        long v = std::strtol(rowCountLine.c_str(), &endp, 10);
        // Accept if all remaining chars are whitespace
        while (endp && *endp == ' ') endp++;
        if (!endp || *endp != '\0')
            throw std::runtime_error(
                std::string("Line ") + std::to_string(lineNum) +
                ": expected row count integer, got '" + rowCountLine + "'");
        if (v <= 0 || v > 65536)
            throw std::runtime_error(
                std::string("Line ") + std::to_string(lineNum) +
                ": row count " + std::to_string(v) + " is out of range (1..65536)");
        num_rows = static_cast<int>(v);
        lines.erase(lines.begin());
        lineNum++;
    }

    // Remaining non-blank lines are pattern rows.
    // Determine column count and widths from the first pattern row.
    // Each channel is separated by '|'.  The last channel may or may not
    // have a trailing '|'.

    IngameFMSong song;
    song.num_rows = num_rows;

    // Filter out blank lines from the data block
    std::vector<std::string> dataLines;
    for (const auto& l : lines)
    {
        if (!l.empty())
            dataLines.push_back(l);
    }

    if (dataLines.empty())
        throw std::runtime_error("Song has row count " + std::to_string(num_rows) + " but no data lines");

    // Determine channel structure from the first data line
    // Split by '|'
    auto split_channels = [](const std::string& row) -> std::vector<std::string>
    {
        std::vector<std::string> cols;
        std::string cur;
        for (char c : row)
        {
            if (c == '|')
            {
                cols.push_back(cur);
                cur.clear();
            }
            else
                cur += c;
        }
        if (!cur.empty())
            cols.push_back(cur);
        return cols;
    };

    std::vector<std::string> firstCols = split_channels(dataLines[0]);
    song.num_channels = static_cast<int>(firstCols.size());

    // Record widths of each channel column from the first row
    std::vector<size_t> colWidths;
    for (const auto& c : firstCols)
        colWidths.push_back(c.size());

    // Validate: each channel must be at least 7 chars wide (note+inst+vol)
    for (int ch = 0; ch < song.num_channels; ch++)
    {
        if (colWidths[ch] < 7)
            throw std::runtime_error(
                std::string("Line ") + std::to_string(lineNum) +
                ": channel " + std::to_string(ch) +
                " column is only " + std::to_string(colWidths[ch]) +
                " chars wide — expected at least 7 (note+inst+vol)");
    }

    // Parse each data line
    int parsedRows = 0;
    for (const auto& line : dataLines)
    {
        if (parsedRows >= num_rows)
            break;  // Ignore extra rows beyond declared count

        IngameFMRow row;
        std::vector<std::string> cols = split_channels(line);

        // Allow rows with fewer columns than the first (pad with empty)
        while (static_cast<int>(cols.size()) < song.num_channels)
            cols.push_back("");

        for (int ch = 0; ch < song.num_channels; ch++)
        {
            // Pad or trim to expected width
            std::string col = cols[ch];
            while (col.size() < colWidths[ch])
                col += '.';

            IngameFMEvent ev = parse_channel_column(col, 0, colWidths[ch], lineNum);
            row.channels.push_back(ev);
        }

        song.rows.push_back(row);
        parsedRows++;
        lineNum++;
    }

    if (parsedRows < num_rows)
        throw std::runtime_error(
            "Song declared " + std::to_string(num_rows) +
            " rows but only " + std::to_string(parsedRows) + " data lines were found");

    return song;
}

// =============================================================================
// IngameFMPlayer — SDL audio-driven playback engine
// =============================================================================

struct IngameFMChannelState
{
    bool   active      = false;   // is a note playing?
    int    instrument  = 0;       // last used instrument id
    int    volume      = 0x7F;    // last used volume (as TL complement: 0=loud)
};

// Per-voice SFX runtime state
struct SfxVoiceState
{
    int  sfx_id           = -1;   // -1 = no SFX holding this voice
    int  priority         = 0;    // 0 = music (lowest), >0 = SFX
    int  ticks_remaining  = 0;    // counts down in rows; 0 = expired
    int  current_row      = 0;    // SFX cursor row
    int  sample_in_row    = 0;    // sample position within current SFX row
    int  samples_per_row  = 0;    // from SfxDef
    bool pending_has_note = false;
    bool pending_is_off   = false;
    int  pending_note     = 0;
    int  pending_inst     = 0;
    int  pending_vol      = 0x7F;
    int  last_instrument  = 0;    // SFX channel state (instrument memory)
    int  last_volume      = 0x7F;

    bool active() const { return sfx_id >= 0 && ticks_remaining > 0; }
};

class IngameFMPlayer
{
public:
    // Maximum channels supported (YM2612 port-0 only: channels 0-2)
    // Extend to 6 by adding port-1 support if needed.
    static constexpr int MAX_CHANNELS = 4;

    IngameFMPlayer()
    {
        // Default: no patches loaded
    }

    // -------------------------------------------------------------------------
    // Configuration (call before play())
    // -------------------------------------------------------------------------

    // Set the song from furnace pattern text.
    // tick_rate : YM timer ticks per second (e.g. 60)
    // speed     : furnace Speed setting (rows advance every `speed` ticks)
    void set_song(const std::string& text, int tick_rate, int speed)
    {
        if (tick_rate <= 0)
            throw std::runtime_error("tick_rate must be > 0");
        if (speed <= 0)
            throw std::runtime_error("speed must be > 0");

        song_       = parse_ingamefm_song(text);
        tick_rate_  = tick_rate;
        speed_      = speed;

        // Samples per row  = (samples_per_second / ticks_per_second) * ticks_per_row
        //                  = (44100 / tick_rate) * speed
        samples_per_row_ = static_cast<int>(
            static_cast<double>(SAMPLE_RATE) / tick_rate_ * speed_);
    }

    // Add an instrument patch by instrument-id.
    // instrument_id corresponds to the hex instrument number in the pattern.
    void add_patch(int instrument_id, const YM2612Patch& patch)
    {
        if (instrument_id < 0 || instrument_id > 255)
            throw std::runtime_error("instrument_id must be 0-255");
        patches_[instrument_id] = patch;
        patches_present_[instrument_id] = true;
    }

    // -------------------------------------------------------------------------
    // Playback modes
    // -------------------------------------------------------------------------

    // play() — blocking. Opens its own SDL audio device, plays until done (or
    // loops forever if loop=true), then closes the device and returns.
    // Pass loop=true to repeat the song indefinitely.
    void play(bool loop = false)
    {
        if (song_.rows.empty())
            throw std::runtime_error("No song loaded — call set_song() first");

        reset_state(loop);

        SDL_AudioSpec desired{};
        desired.freq     = SAMPLE_RATE;
        desired.format   = AUDIO_S16SYS;
        desired.channels = 2;
        desired.samples  = 512;
        desired.callback = s_audio_callback;
        desired.userdata = this;

        SDL_AudioSpec obtained{};
        SDL_AudioDeviceID dev = SDL_OpenAudioDevice(nullptr, 0, &desired, &obtained, 0);
        if (dev == 0)
            throw std::runtime_error(std::string("SDL_OpenAudioDevice failed: ") + SDL_GetError());

        SDL_PauseAudioDevice(dev, 0);

        while (!finished_.load())
            SDL_Delay(10);

        SDL_CloseAudioDevice(dev);
    }

    // start() — non-blocking. Resets playback state and begins streaming into
    // the provided (already open, already unpaused) SDL audio device.
    // The caller owns the device and is responsible for closing it.
    // Pass loop=true to repeat the song indefinitely.
    void start(SDL_AudioDeviceID dev, bool loop = false)
    {
        if (song_.rows.empty())
            throw std::runtime_error("No song loaded — call set_song() first");

        SDL_LockAudioDevice(dev);
        reset_state(loop);
        SDL_UnlockAudioDevice(dev);
    }

    // stop() — immediately silences this player on the given device.
    // Call with the audio device locked, or pass the device to lock internally.
    void stop(SDL_AudioDeviceID dev)
    {
        SDL_LockAudioDevice(dev);
        finished_.store(true);
        for (int ch = 0; ch < MAX_CHANNELS; ch++)
            if (ym_) ym_->key_off(ch);
        SDL_UnlockAudioDevice(dev);
    }

    // is_finished() — true when the song has ended (only meaningful when not looping)
    bool is_finished() const { return finished_.load(); }

    // chip() — direct access to the YM2612 chip.
    // Use this to play notes on channels not used by the song (e.g. ch 2 for SFX).
    // Always call with the SDL audio device locked.
    IngameFMChip* chip() { return ym_.get(); }

    // -------------------------------------------------------------------------
    // SFX API
    // -------------------------------------------------------------------------

    // sfx_reserve(n) — designate the last N channels as SFX-capable voices.
    // Music may still play on them when no SFX holds them.
    // Must be called before start().
    void sfx_reserve(int n)
    {
        if (n < 0 || n > MAX_CHANNELS)
            throw std::runtime_error("sfx_reserve: n must be 0.." + std::to_string(MAX_CHANNELS));
        sfx_voices_ = n;
    }

    // sfx_define(id, pattern, tick_rate, speed) — pre-parse an SFX pattern.
    // id        : caller-chosen integer key (0-255)
    // pattern   : single-channel Furnace pattern text (same format as set_song)
    // tick_rate : ticks per second for this SFX (e.g. 60)
    // speed     : rows per tick group (e.g. 6)
    // May be called at any time (even while audio is running).
    void sfx_define(int id, const std::string& pattern, int tick_rate, int speed)
    {
        if (id < 0 || id > 255)
            throw std::runtime_error("sfx_define: id must be 0-255");
        if (tick_rate <= 0 || speed <= 0)
            throw std::runtime_error("sfx_define: tick_rate and speed must be > 0");

        SfxDef def;
        def.song            = parse_ingamefm_song(pattern);
        def.tick_rate       = tick_rate;
        def.speed           = speed;
        def.samples_per_row = static_cast<int>(
            static_cast<double>(SAMPLE_RATE) / tick_rate * speed);
        sfx_defs_[id]         = std::move(def);
        sfx_defs_present_[id] = true;
    }

    // sfx_play(id, priority, duration_ticks) — trigger an SFX immediately.
    // Scans reserved voices right-to-left; picks the rightmost free voice,
    // or the rightmost voice with strictly lower priority.
    // If no suitable voice is found, the call is silently ignored.
    // Must be called with the SDL audio device locked.
    void sfx_play(int id, int priority, int duration_ticks)
    {
        if (!sfx_defs_present_[id]) return;
        if (sfx_voices_ == 0)       return;
        if (!ym_)                   return;

        const SfxDef& def  = sfx_defs_[id];
        int first_sfx_ch   = MAX_CHANNELS - sfx_voices_;

        int best_ch        = -1;
        int best_priority  = priority;  // we need strictly lower than this

        // Scan right-to-left: prefer rightmost free, then rightmost lower-priority
        for (int ch = MAX_CHANNELS - 1; ch >= first_sfx_ch; --ch)
        {
            SfxVoiceState& v = sfx_voice_[ch];
            int cur_prio = v.active() ? v.priority : 0;

            if (!v.active())
            {
                // Free voice — ideal, take it immediately
                best_ch       = ch;
                best_priority = -1;  // can't beat free
                break;
            }
            else if (cur_prio < best_priority)
            {
                best_ch       = ch;
                best_priority = cur_prio;
            }
        }

        if (best_ch < 0) return;  // all reserved voices have equal/higher priority

        // Evict whatever is on best_ch
        ym_->key_off(best_ch);
        ch_state_[best_ch].active = false;

        // Initialise SFX voice
        SfxVoiceState& v   = sfx_voice_[best_ch];
        v.sfx_id           = id;
        v.priority         = priority;
        v.ticks_remaining  = duration_ticks;
        v.current_row      = 0;
        v.sample_in_row    = 0;
        v.samples_per_row  = def.samples_per_row;
        v.pending_has_note = false;
        v.pending_is_off   = false;
        v.last_instrument  = 0;
        v.last_volume      = 0x7F;

        // Start immediately: eviction already called key_off, so skip it in
        // sfx_process_row and go straight to commit_keyon.
        sfx_process_row(best_ch, 0, /*skip_keyoff=*/true);
        sfx_commit_keyon(best_ch);
        v.sample_in_row    = KEY_OFF_GAP_SAMPLES;  // already committed
    }

    // Audio callback — wire this into SDL_AudioSpec::callback when sharing a device.
    static void s_audio_callback(void* userdata, Uint8* stream, int len)
    {
        static_cast<IngameFMPlayer*>(userdata)->audio_callback(
            reinterpret_cast<int16_t*>(stream), len / 4);
    }

    static constexpr int SAMPLE_RATE = 44100;

private:
    // -------------------------------------------------------------------------
    // SFX private data
    // -------------------------------------------------------------------------

    struct SfxDef
    {
        IngameFMSong song;
        int tick_rate       = 60;
        int speed           = 6;
        int samples_per_row = 0;
    };

    std::array<SfxDef,  256> sfx_defs_{};
    std::array<bool,    256> sfx_defs_present_{};
    int                      sfx_voices_ = 0;   // last N channels are SFX-eligible
    std::array<SfxVoiceState, MAX_CHANNELS> sfx_voice_{};

    // -------------------------------------------------------------------------
    // Internal state
    // -------------------------------------------------------------------------

    IngameFMSong song_;
    int         tick_rate_       = 60;
    int         speed_           = 6;
    int         samples_per_row_ = 0;

    std::unique_ptr<IngameFMChip> ym_;

    int  current_row_   = 0;
    int  sample_in_row_ = 0;
    bool loop_          = false;
    std::atomic<bool> finished_{ false };

    std::array<IngameFMChannelState, MAX_CHANNELS> ch_state_{};

    // Patch map
    std::array<YM2612Patch, 256>  patches_{};
    std::array<bool, 256>         patches_present_{};

    void reset_state(bool loop)
    {
        loop_          = loop;
        current_row_   = 0;
        sample_in_row_ = 0;
        finished_.store(false);
        for (auto& ch : ch_state_)
            ch = IngameFMChannelState{};
        for (auto& p : pending_)
            p = {};
        for (auto& v : sfx_voice_)
            v = SfxVoiceState{};
        ym_ = std::make_unique<IngameFMChip>();
        // Row 0: key_off (chip is silent, so no EG to worry about),
        // then immediately commit key_on — no gap needed at startup.
        process_row(0);
        commit_keyon();
        // Pretend we are already past the gap so the callback doesn't fire
        // commit_keyon() a second time for row 0.
        sample_in_row_ = KEY_OFF_GAP_SAMPLES;
    }

    // -------------------------------------------------------------------------
    // Row processing — split into two phases so the EG has time to reach zero.
    //
    // phase_keyoff(): issue key_off for every channel that has a new note or OFF
    //                 coming this row. Called BEFORE generating samples.
    // phase_keyon():  load patch, set frequency, key_on. Called AFTER a short
    //                 gap of generated samples so the EG starts from silence.
    // -------------------------------------------------------------------------

    struct PendingNote
    {
        bool  has_note  = false;
        bool  is_off    = false;
        int   midi_note = 0;
        int   instId    = 0;
        int   volume    = 0x7F;  // 0x00=silent, 0x7F=loudest (Furnace scale)
    };
    std::array<PendingNote, MAX_CHANNELS> pending_{};

    void process_row(int rowIdx)
    {
        if (rowIdx >= static_cast<int>(song_.rows.size()))
            return;

        const IngameFMRow& row = song_.rows[rowIdx];
        int numCh = std::min(static_cast<int>(row.channels.size()), MAX_CHANNELS);

        // Phase 1 — collect events, update channel state, issue key_off
        for (int ch = 0; ch < numCh; ch++)
        {
            const IngameFMEvent& ev = row.channels[ch];
            pending_[ch] = {};

            if (ev.instrument >= 0)
                ch_state_[ch].instrument = ev.instrument;
            if (ev.volume >= 0)
                ch_state_[ch].volume = ev.volume;

            // Don't interrupt an SFX-held voice with music key_off
            if (sfx_voice_[ch].active()) continue;

            if (ev.note == NOTE_OFF)
            {
                ym_->key_off(ch);
                ch_state_[ch].active = false;
                pending_[ch].is_off  = true;
            }
            else if (ev.note >= 0)
            {
                // Key off NOW so the EG has the inter-note sample gap to decay
                ym_->key_off(ch);
                ch_state_[ch].active   = false;
                pending_[ch].has_note  = true;
                pending_[ch].midi_note = ev.note;
                pending_[ch].instId    = ch_state_[ch].instrument;
                pending_[ch].volume    = ch_state_[ch].volume;
            }
        }
        // Phase 2 (key_on) is called from audio_callback after the gap samples
    }

    // Called after KEY_OFF_GAP_SAMPLES have been generated for this row.
    void commit_keyon()
    {
        for (int ch = 0; ch < MAX_CHANNELS; ch++)
        {
            // Don't let music key_on a channel that SFX currently owns
            if (sfx_voice_[ch].active()) continue;

            if (!pending_[ch].has_note)
                continue;

            int instId = pending_[ch].instId;
            if (patches_present_[instId])
            {
                // Apply volume by scaling carrier TL on a local patch copy.
                // Volume 0x7F = full loudness (no TL increase).
                // Volume 0x00 = silent (maximum TL increase = 127).
                // We work on a copy so the master patch struct is never mutated.
                YM2612Patch p = patches_[instId];
                int vol = pending_[ch].volume;               // 0-127
                int tl_add = ((0x7F - vol) * 127) / 0x7F;   // 0 at full vol, 127 at silence

                // Carrier flags per algorithm (OPN standard)
                bool isCarrier[4] = { false, false, false, false };
                switch (p.ALG)
                {
                    case 0: case 1: case 2: case 3:
                        isCarrier[3] = true; break;
                    case 4:
                        isCarrier[1] = true; isCarrier[3] = true; break;
                    case 5: case 6:
                        isCarrier[1] = true; isCarrier[2] = true; isCarrier[3] = true; break;
                    case 7:
                        isCarrier[0] = isCarrier[1] = isCarrier[2] = isCarrier[3] = true; break;
                }
                for (int op = 0; op < 4; op++)
                    if (isCarrier[op])
                        p.op[op].TL = std::min(127, p.op[op].TL + tl_add);

                ym_->load_patch(p, ch);
            }

            double hz = IngameFMChip::midi_to_hz(pending_[ch].midi_note);
            ym_->set_frequency(ch, hz, 0);
            ym_->key_on(ch);
            ch_state_[ch].active  = true;
            pending_[ch].has_note = false;
        }
    }

    // -------------------------------------------------------------------------
    // SFX per-voice helpers
    // -------------------------------------------------------------------------

    // Process one SFX row for a single voice/channel.
    // skip_keyoff: true when called from sfx_play (eviction already did key_off)
    void sfx_process_row(int ch, int rowIdx, bool skip_keyoff = false)
    {
        SfxVoiceState& v   = sfx_voice_[ch];
        const SfxDef&  def = sfx_defs_[v.sfx_id];

        v.pending_has_note = false;
        v.pending_is_off   = false;

        if (rowIdx >= static_cast<int>(def.song.rows.size())) return;

        const IngameFMEvent& ev = def.song.rows[rowIdx].channels[0];

        if (ev.instrument >= 0) v.last_instrument = ev.instrument;
        if (ev.volume     >= 0) v.last_volume     = ev.volume;

        if (ev.note == NOTE_OFF)
        {
            if (!skip_keyoff) ym_->key_off(ch);
            v.pending_is_off = true;
        }
        else if (ev.note >= 0)
        {
            if (!skip_keyoff) ym_->key_off(ch);
            v.pending_has_note = true;
            v.pending_note     = ev.note;
            v.pending_inst     = v.last_instrument;
            v.pending_vol      = v.last_volume;
        }
    }

    // Commit key_on for a single SFX voice after the gap.
    void sfx_commit_keyon(int ch)
    {
        SfxVoiceState& v = sfx_voice_[ch];
        if (!v.pending_has_note) return;

        int instId = v.pending_inst;
        if (patches_present_[instId])
        {
            YM2612Patch p  = patches_[instId];
            int vol        = v.pending_vol;
            int tl_add     = ((0x7F - vol) * 127) / 0x7F;

            bool isCarrier[4] = { false, false, false, false };
            switch (p.ALG)
            {
                case 0: case 1: case 2: case 3:
                    isCarrier[3] = true; break;
                case 4:
                    isCarrier[1] = true; isCarrier[3] = true; break;
                case 5: case 6:
                    isCarrier[1] = isCarrier[2] = isCarrier[3] = true; break;
                case 7:
                    isCarrier[0] = isCarrier[1] = isCarrier[2] = isCarrier[3] = true; break;
            }
            for (int op = 0; op < 4; op++)
                if (isCarrier[op])
                    p.op[op].TL = std::min(127, p.op[op].TL + tl_add);

            ym_->load_patch(p, ch);
        }

        double hz = IngameFMChip::midi_to_hz(v.pending_note);
        ym_->set_frequency(ch, hz, 0);
        ym_->key_on(ch);
        v.pending_has_note = false;
    }

    // Advance one SFX voice by `samples` audio samples.
    // Called from audio_callback for every SFX-active voice each buffer chunk.
    void sfx_tick_voice(int ch, int samples)
    {
        SfxVoiceState& v = sfx_voice_[ch];
        if (!v.active()) return;

        const SfxDef& def = sfx_defs_[v.sfx_id];
        int remaining = samples;

        while (remaining > 0)
        {
            bool in_gap     = v.sample_in_row < KEY_OFF_GAP_SAMPLES;
            int  next_bound = in_gap ? KEY_OFF_GAP_SAMPLES : v.samples_per_row;
            int  to_advance = std::min(remaining, next_bound - v.sample_in_row);

            v.sample_in_row += to_advance;
            remaining       -= to_advance;

            // Crossed gap → key_on
            if (in_gap && v.sample_in_row >= KEY_OFF_GAP_SAMPLES)
                sfx_commit_keyon(ch);

            // Crossed row boundary → advance SFX row
            if (v.sample_in_row >= v.samples_per_row)
            {
                v.sample_in_row = 0;
                v.ticks_remaining--;

                if (v.ticks_remaining <= 0)
                {
                    // SFX expired — silence and release the voice
                    ym_->key_off(ch);
                    v = SfxVoiceState{};
                    return;
                }

                v.current_row++;
                if (v.current_row >= static_cast<int>(def.song.rows.size()))
                    v.current_row = 0;  // loop SFX pattern

                sfx_process_row(ch, v.current_row);
            }
        }
    }

    // -------------------------------------------------------------------------
    // SDL audio callback
    // -------------------------------------------------------------------------

    // Samples generated after key_off before key_on (~1ms at 44100 Hz).
    // This gives ymfm's EG enough time to reach full attenuation so the next
    // note always starts from silence rather than mid-decay level.
    static constexpr int KEY_OFF_GAP_SAMPLES = 44;

    void audio_callback(int16_t* stream, int samples)
    {
        if (finished_.load())
        {
            std::memset(stream, 0, samples * 4);
            return;
        }

        int remaining = samples;
        int16_t* out  = stream;

        while (remaining > 0)
        {
            // How many samples remain in the gap / in the row body?
            int pos_in_row          = sample_in_row_;
            bool in_gap             = pos_in_row < KEY_OFF_GAP_SAMPLES;
            int gap_end             = KEY_OFF_GAP_SAMPLES;
            int row_end             = samples_per_row_;

            int next_boundary = in_gap ? gap_end : row_end;
            int to_generate   = std::min(remaining, next_boundary - pos_in_row);

            ym_->generate(out, to_generate);

            // Advance SFX voice cursors by the same sample count.
            // (chip audio was already generated above; sfx_tick_voice only
            //  manages row/gap cursors and calls key_off/key_on as needed.)
            for (int ch = MAX_CHANNELS - sfx_voices_; ch < MAX_CHANNELS; ++ch)
                sfx_tick_voice(ch, to_generate);

            out            += to_generate * 2;
            remaining      -= to_generate;
            sample_in_row_ += to_generate;

            // Crossed the gap boundary → time to key_on pending notes
            if (in_gap && sample_in_row_ >= KEY_OFF_GAP_SAMPLES)
                commit_keyon();

            // Crossed the row boundary → advance to next row
            if (sample_in_row_ >= samples_per_row_)
            {
                sample_in_row_ = 0;
                current_row_++;

                if (current_row_ >= static_cast<int>(song_.rows.size()))
                {
                    if (loop_)
                    {
                        // Furnace loop behaviour: just reset the cursor.
                        // No blanket key_off — the song is responsible for
                        // silencing channels it wants quiet at the loop point
                        // (use an OFF event on the last row). This also means
                        // SFX channels not owned by the song (e.g. ch2 guitar)
                        // are never interrupted by a loop reset.
                        // ch_state_ is also left intact so instrument/volume
                        // memory carries across the loop boundary naturally.
                        for (auto& p : pending_)
                            p = {};
                        current_row_ = 0;
                    }
                    else
                    {
                        std::memset(out, 0, remaining * 4);
                        for (int ch = 0; ch < MAX_CHANNELS; ch++)
                            ym_->key_off(ch);
                        finished_.store(true);
                        return;
                    }
                }

                process_row(current_row_);
            }
        }
    }
};


