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

class IngameFMPlayer
{
public:
    // Maximum channels supported (YM2612 port-0 only: channels 0-2)
    // Extend to 6 by adding port-1 support if needed.
    static constexpr int MAX_CHANNELS = 3;

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
    // Playback — call play() from your main thread.
    // It opens SDL audio, streams the song, then returns.
    // -------------------------------------------------------------------------

    void play()
    {
        if (song_.rows.empty())
            throw std::runtime_error("No song loaded — call set_song() first");

        // Reset state
        current_row_        = 0;
        sample_in_row_      = 0;
        finished_           = false;
        for (auto& ch : ch_state_)
            ch = IngameFMChannelState{};

        // Reset YM chip
        ym_ = std::make_unique<IngameFMChip>();

        // Immediately process row 0 (before generating any audio)
        process_row(current_row_);

        // Open SDL audio
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

        // Wait for playback to finish
        while (!finished_.load())
            SDL_Delay(10);

        SDL_CloseAudioDevice(dev);
    }

    // Non-blocking alternative: call update() repeatedly from your loop.
    // You must supply your own SDL audio device opened with s_audio_callback / this.
    // Returns true while playing, false when done.
    bool update() { return !finished_.load(); }

    // Expose callback for use when opening your own SDL audio device
    static void s_audio_callback(void* userdata, Uint8* stream, int len)
    {
        static_cast<IngameFMPlayer*>(userdata)->audio_callback(
            reinterpret_cast<int16_t*>(stream), len / 4);
    }

    static constexpr int SAMPLE_RATE = 44100;

private:
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
    std::atomic<bool> finished_{ false };

    std::array<IngameFMChannelState, MAX_CHANNELS> ch_state_{};

    // Patch map
    std::array<YM2612Patch, 256>  patches_{};
    std::array<bool, 256>         patches_present_{};

    // -------------------------------------------------------------------------
    // Row processing (called with audio lock implicitly held via callback)
    // -------------------------------------------------------------------------

    void process_row(int rowIdx)
    {
        if (rowIdx >= static_cast<int>(song_.rows.size()))
            return;

        const IngameFMRow& row = song_.rows[rowIdx];
        int numCh = std::min(static_cast<int>(row.channels.size()), MAX_CHANNELS);

        for (int ch = 0; ch < numCh; ch++)
        {
            const IngameFMEvent& ev = row.channels[ch];

            // Update instrument if specified
            if (ev.instrument >= 0)
            {
                ch_state_[ch].instrument = ev.instrument;
                // Load patch into YM channel
                if (patches_present_[ev.instrument])
                    ym_->load_patch(patches_[ev.instrument], ch);
            }

            // Update volume if specified
            // Furnace volume 0x00-0x7F maps to TL: lower TL = louder
            // We store as raw furnace volume and convert:
            //   YM TL = 127 - ingamefm_vol  (so 0x7F = loudest = TL 0)
            if (ev.volume >= 0)
                ch_state_[ch].volume = ev.volume;

            // Handle note events
            if (ev.note == NOTE_OFF)
            {
                ym_->key_off(ch);
                ch_state_[ch].active = false;
            }
            else if (ev.note >= 0)
            {
                // Key off first to retrigger
                ym_->key_off(ch);

                // Apply volume: set OP TL for the output operators
                // For simplicity, we apply volume as an overall TL offset on OP3/OP4
                // (the "carrier" operators in most algorithms).
                // A proper implementation would query the algorithm, but this is a
                // good general approximation.
                apply_volume(ch, ch_state_[ch].volume);

                // Set frequency
                double hz = IngameFMChip::midi_to_hz(ev.note);
                ym_->set_frequency(ch, hz, 0);

                // Key on
                ym_->key_on(ch);
                ch_state_[ch].active = true;
            }
            // NOTE_NONE: no change — channel continues as-is
        }
    }

    // Apply furnace volume (0-127) to channel ch.
    // We modify TL of output operators based on the patch's algorithm.
    void apply_volume(int ch, int furnaceVol)
    {
        int instId = ch_state_[ch].instrument;
        if (!patches_present_[instId])
            return;

        const YM2612Patch& p = patches_[instId];

        // Carriers depend on algorithm. For ALG 4-7 all slots are carriers;
        // for ALG 0-3 only OP4 (slot index 3) is a carrier.
        // We apply a simple TL offset: TL = base_TL + (127 - furnaceVol)
        // clamped to [0,127].
        // This only modifies carriers (OP4 always; OP3 for ALG >= 4; etc.)

        // Determine which ops are carriers for this algorithm
        // Furnace/OPN carrier table:
        //  ALG 0: OP4
        //  ALG 1: OP4
        //  ALG 2: OP4
        //  ALG 3: OP4
        //  ALG 4: OP2, OP4
        //  ALG 5: OP2, OP3, OP4
        //  ALG 6: OP2, OP3, OP4
        //  ALG 7: OP1, OP2, OP3, OP4
        bool isCarrier[4] = { false, false, false, false };
        switch (p.ALG)
        {
            case 0: case 1: case 2: case 3:
                isCarrier[3] = true;
                break;
            case 4:
                isCarrier[1] = true; isCarrier[3] = true;
                break;
            case 5: case 6:
                isCarrier[1] = true; isCarrier[2] = true; isCarrier[3] = true;
                break;
            case 7:
                isCarrier[0] = isCarrier[1] = isCarrier[2] = isCarrier[3] = true;
                break;
        }

        const int slotMap[4] = { 0, 2, 1, 3 };
        int volumeOffset = 127 - furnaceVol; // 0=loudest

        for (int patchOp = 0; patchOp < 4; patchOp++)
        {
            if (!isCarrier[patchOp])
                continue;

            int hwSlot = slotMap[patchOp];
            int baseTL = p.op[patchOp].TL;
            int newTL  = std::min(127, baseTL + volumeOffset / 4); // gentle scaling
            ym_->write(0, 0x40 + hwSlot * 4 + ch, static_cast<uint8_t>(newTL & 0x7F));
        }
    }

    // -------------------------------------------------------------------------
    // SDL audio callback
    // -------------------------------------------------------------------------

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
            int samples_left_in_row = samples_per_row_ - sample_in_row_;
            int to_generate = std::min(remaining, samples_left_in_row);

            ym_->generate(out, to_generate);
            out              += to_generate * 2;
            remaining        -= to_generate;
            sample_in_row_   += to_generate;

            if (sample_in_row_ >= samples_per_row_)
            {
                sample_in_row_ = 0;
                current_row_++;

                if (current_row_ >= static_cast<int>(song_.rows.size()))
                {
                    // Song finished — silence remaining output
                    std::memset(out, 0, remaining * 4);
                    // Key off all channels
                    for (int ch = 0; ch < MAX_CHANNELS; ch++)
                        ym_->key_off(ch);
                    finished_.store(true);
                    return;
                }

                process_row(current_row_);
            }
        }
    }
};


