#pragma once
#include <sstream>
// =============================================================================
// ingamefm_player.h  — part of ingamefm (header-only)
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

static constexpr int NOTE_NONE = -1;
static constexpr int NOTE_OFF  = -2;

struct IngameFMEvent
{
    int note;
    int instrument;
    int volume;
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

static std::string trim_right(const std::string& s)
{
    size_t end = s.find_last_not_of(" \t\r\n");
    return (end == std::string::npos) ? "" : s.substr(0, end + 1);
}

static int parse_note_field(const char* nc, int line_num)
{
    if (nc[0] == '.' && nc[1] == '.' && nc[2] == '.')
        return NOTE_NONE;
    if (nc[0] == 'O' && nc[1] == 'F' && nc[2] == 'F')
        return NOTE_OFF;
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
    if (nc[1] == '#')      semitone++;
    else if (nc[1] == '-') {}
    else
        throw std::runtime_error(
            std::string("Line ") + std::to_string(line_num) +
            ": expected '#' or '-' for accidental in note '" + std::string(nc, 3) + "'");
    if (nc[2] < '0' || nc[2] > '9')
        throw std::runtime_error(
            std::string("Line ") + std::to_string(line_num) +
            ": expected octave digit 0-9 in note '" + std::string(nc, 3) + "'");
    int octave   = nc[2] - '0';
    int midiNote = 12 + octave * 12 + semitone;
    return midiNote;
}

static int parse_hex2(const char* p, int line_num, const char* field_name)
{
    if (p[0] == '.' && p[1] == '.')
        return -1;
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
    std::string col = row.substr(colStart, colWidth);
    while (col.size() < colWidth)
        col += '.';
    ev.note = parse_note_field(col.c_str(), line_num);
    if (col.size() >= 5)
        ev.instrument = parse_hex2(col.c_str() + 3, line_num, "instrument");
    if (col.size() >= 7)
        ev.volume = parse_hex2(col.c_str() + 5, line_num, "volume");
    return ev;
}

static IngameFMSong parse_ingamefm_song(const std::string& text)
{
    std::vector<std::string> lines;
    {
        std::istringstream ss(text);
        std::string line;
        while (std::getline(ss, line))
            lines.push_back(trim_right(line));
    }
    while (!lines.empty() && lines.front().empty())
        lines.erase(lines.begin());
    if (lines.empty())
        throw std::runtime_error("Song text is empty");
    int lineNum = 1;
    if (lines[0].find("org.tildearrow.furnace") != std::string::npos)
    {
        lines.erase(lines.begin());
        lineNum++;
    }
    while (!lines.empty() && lines.front().empty())
    {
        lines.erase(lines.begin());
        lineNum++;
    }
    if (lines.empty())
        throw std::runtime_error("Song text has no content after header");
    int num_rows = 0;
    {
        const std::string& rowCountLine = lines[0];
        char* endp = nullptr;
        long v = std::strtol(rowCountLine.c_str(), &endp, 10);
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
    IngameFMSong song;
    song.num_rows = num_rows;
    std::vector<std::string> dataLines;
    for (const auto& l : lines)
        if (!l.empty()) dataLines.push_back(l);
    if (dataLines.empty())
        throw std::runtime_error("Song has row count " + std::to_string(num_rows) + " but no data lines");
    auto split_channels = [](const std::string& row) -> std::vector<std::string>
    {
        std::vector<std::string> cols;
        std::string cur;
        for (char c : row)
        {
            if (c == '|') { cols.push_back(cur); cur.clear(); }
            else cur += c;
        }
        if (!cur.empty()) cols.push_back(cur);
        return cols;
    };
    std::vector<std::string> firstCols = split_channels(dataLines[0]);
    song.num_channels = static_cast<int>(firstCols.size());
    std::vector<size_t> colWidths;
    for (const auto& c : firstCols)
        colWidths.push_back(c.size());
    for (int ch = 0; ch < song.num_channels; ch++)
        if (colWidths[ch] < 7)
            throw std::runtime_error(
                std::string("Line ") + std::to_string(lineNum) +
                ": channel " + std::to_string(ch) +
                " column is only " + std::to_string(colWidths[ch]) +
                " chars wide — expected at least 7 (note+inst+vol)");
    int parsedRows = 0;
    for (const auto& line : dataLines)
    {
        if (parsedRows >= num_rows) break;
        IngameFMRow row;
        std::vector<std::string> cols = split_channels(line);
        while (static_cast<int>(cols.size()) < song.num_channels)
            cols.push_back("");
        for (int ch = 0; ch < song.num_channels; ch++)
        {
            std::string col = cols[ch];
            while (col.size() < colWidths[ch]) col += '.';
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

struct IngameFMChannelState
{
    bool   active      = false;
    int    instrument  = 0;
    int    volume      = 0x7F;
};

struct SfxVoiceState
{
    int  sfx_id           = -1;
    int  priority         = 0;
    int  ticks_remaining  = 0;
    int  current_row      = 0;
    int  sample_in_row    = 0;
    int  samples_per_row  = 0;
    bool pending_has_note = false;
    bool pending_is_off   = false;
    int  pending_note     = 0;
    int  pending_inst     = 0;
    int  pending_vol      = 0x7F;
    int  last_instrument  = 0;
    int  last_volume      = 0x7F;

    bool active() const { return sfx_id >= 0 && ticks_remaining > 0; }
};

class IngameFMPlayer
{
public:
    static constexpr int MAX_CHANNELS = 6;

    IngameFMPlayer() {}

    // Volume control — 0.0 (silent) .. 1.0 (full).
    // Music and SFX are rendered into separate passes using YM2612 panning
    // registers as channel masks, then mixed with independent scalars.
    // Thread-safe — no audio lock needed.
    void set_music_volume(float v) { music_vol_.store(std::max(0.f, std::min(1.f, v))); }
    void set_sfx_volume  (float v) { sfx_vol_  .store(std::max(0.f, std::min(1.f, v))); }

    void set_song(const std::string& text, int tick_rate, int speed)
    {
        if (tick_rate <= 0) throw std::runtime_error("tick_rate must be > 0");
        if (speed <= 0)     throw std::runtime_error("speed must be > 0");
        song_       = parse_ingamefm_song(text);
        tick_rate_  = tick_rate;
        speed_      = speed;
        samples_per_row_ = static_cast<int>(
            static_cast<double>(SAMPLE_RATE) / tick_rate_ * speed_);
    }

    void add_patch(int instrument_id, const YM2612Patch& patch)
    {
        if (instrument_id < 0 || instrument_id > 255)
            throw std::runtime_error("instrument_id must be 0-255");
        patches_[instrument_id] = patch;
        patches_present_[instrument_id] = true;
    }

    void play(bool loop = false)
    {
        if (song_.rows.empty()) throw std::runtime_error("No song loaded — call set_song() first");
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
        while (!finished_.load()) SDL_Delay(10);
        SDL_CloseAudioDevice(dev);
    }

    void start(SDL_AudioDeviceID dev, bool loop = false)
    {
        if (song_.rows.empty()) throw std::runtime_error("No song loaded — call set_song() first");
        SDL_LockAudioDevice(dev);
        reset_state(loop);
        SDL_UnlockAudioDevice(dev);
    }

    void stop(SDL_AudioDeviceID dev)
    {
        SDL_LockAudioDevice(dev);
        finished_.store(true);
        for (int ch = 0; ch < MAX_CHANNELS; ch++)
            if (ym_) ym_->key_off(ch);
        SDL_UnlockAudioDevice(dev);
    }

    bool is_finished() const { return finished_.load(); }
    IngameFMChip* chip() { return ym_.get(); }

    void sfx_reserve(int n)
    {
        if (n < 0 || n > MAX_CHANNELS)
            throw std::runtime_error("sfx_reserve: n must be 0.." + std::to_string(MAX_CHANNELS));
        sfx_voices_ = n;
    }

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

    void sfx_play(int id, int priority, int duration_ticks)
    {
        if (!sfx_defs_present_[id]) return;
        if (sfx_voices_ == 0)       return;
        if (!ym_)                   return;
        const SfxDef& def  = sfx_defs_[id];
        int first_sfx_ch   = MAX_CHANNELS - sfx_voices_;
        int best_ch        = -1;
        int best_priority  = priority;
        for (int ch = MAX_CHANNELS - 1; ch >= first_sfx_ch; --ch)
        {
            SfxVoiceState& v = sfx_voice_[ch];
            if (!v.active()) { best_ch = ch; best_priority = -1; break; }
            else if (v.priority < best_priority) { best_ch = ch; best_priority = v.priority; }
        }
        if (best_ch < 0) return;
        ym_->key_off(best_ch);
        ch_state_[best_ch].active = false;
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
        sfx_process_row(best_ch, 0, true);
        sfx_commit_keyon(best_ch);
        v.sample_in_row    = KEY_OFF_GAP_SAMPLES;
    }

    static void s_audio_callback(void* userdata, Uint8* stream, int len)
    {
        static_cast<IngameFMPlayer*>(userdata)->audio_callback(
            reinterpret_cast<int16_t*>(stream), len / 4);
    }

    static constexpr int SAMPLE_RATE = 44100;

private:
    struct SfxDef
    {
        IngameFMSong song;
        int tick_rate       = 60;
        int speed           = 6;
        int samples_per_row = 0;
    };

    std::array<SfxDef,  256> sfx_defs_{};
    std::array<bool,    256> sfx_defs_present_{};
    int                      sfx_voices_ = 0;
    std::array<SfxVoiceState, MAX_CHANNELS> sfx_voice_{};

    IngameFMSong song_;
    int         tick_rate_       = 60;
    int         speed_           = 6;
    int         samples_per_row_ = 0;

    std::unique_ptr<IngameFMChip> ym_;

    std::atomic<float> music_vol_{ 1.0f };
    std::atomic<float> sfx_vol_  { 1.0f };

    int  current_row_   = 0;
    int  sample_in_row_ = 0;
    bool loop_          = false;
    std::atomic<bool> finished_{ false };

    std::array<IngameFMChannelState, MAX_CHANNELS> ch_state_{};

    std::array<YM2612Patch, 256>  patches_{};
    std::array<bool, 256>         patches_present_{};

    void reset_state(bool loop)
    {
        loop_          = loop;
        current_row_   = 0;
        sample_in_row_ = 0;
        finished_.store(false);
        for (auto& ch : ch_state_) ch = IngameFMChannelState{};
        for (auto& p : pending_)   p = {};
        for (auto& v : sfx_voice_) v = SfxVoiceState{};
        ym_ = std::make_unique<IngameFMChip>();
        process_row(0);
        commit_keyon();
        sample_in_row_ = KEY_OFF_GAP_SAMPLES;
    }

    struct PendingNote
    {
        bool  has_note  = false;
        bool  is_off    = false;
        int   midi_note = 0;
        int   instId    = 0;
        int   volume    = 0x7F;
    };
    std::array<PendingNote, MAX_CHANNELS> pending_{};

    void process_row(int rowIdx)
    {
        if (rowIdx >= static_cast<int>(song_.rows.size())) return;
        const IngameFMRow& row = song_.rows[rowIdx];
        int numCh = std::min(static_cast<int>(row.channels.size()), MAX_CHANNELS);
        for (int ch = 0; ch < numCh; ch++)
        {
            const IngameFMEvent& ev = row.channels[ch];
            pending_[ch] = {};
            if (ev.instrument >= 0) ch_state_[ch].instrument = ev.instrument;
            if (ev.volume >= 0)     ch_state_[ch].volume = ev.volume;
            if (sfx_voice_[ch].active()) continue;
            if (ev.note == NOTE_OFF)
            {
                ym_->key_off(ch);
                ch_state_[ch].active = false;
                pending_[ch].is_off  = true;
            }
            else if (ev.note >= 0)
            {
                ym_->key_off(ch);
                ch_state_[ch].active   = false;
                pending_[ch].has_note  = true;
                pending_[ch].midi_note = ev.note;
                pending_[ch].instId    = ch_state_[ch].instrument;
                pending_[ch].volume    = ch_state_[ch].volume;
            }
        }
    }

    void commit_keyon()
    {
        for (int ch = 0; ch < MAX_CHANNELS; ch++)
        {
            if (sfx_voice_[ch].active()) continue;
            if (!pending_[ch].has_note) continue;
            int instId = pending_[ch].instId;
            if (patches_present_[instId])
            {
                YM2612Patch p = patches_[instId];
                int vol = pending_[ch].volume;
                int tl_add = ((0x7F - vol) * 127) / 0x7F;
                bool isCarrier[4] = { false, false, false, false };
                switch (p.ALG)
                {
                    case 0: case 1: case 2: case 3: isCarrier[3] = true; break;
                    case 4: isCarrier[1] = true; isCarrier[3] = true; break;
                    case 5: case 6: isCarrier[1] = true; isCarrier[2] = true; isCarrier[3] = true; break;
                    case 7: isCarrier[0] = isCarrier[1] = isCarrier[2] = isCarrier[3] = true; break;
                }
                for (int op = 0; op < 4; op++)
                    if (isCarrier[op]) p.op[op].TL = std::min(127, p.op[op].TL + tl_add);
                ym_->load_patch(p, ch);
            }
            double hz = IngameFMChip::midi_to_hz(pending_[ch].midi_note);
            ym_->set_frequency(ch, hz, 0);
            ym_->key_on(ch);
            ch_state_[ch].active  = true;
            pending_[ch].has_note = false;
        }
    }

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
                case 0: case 1: case 2: case 3: isCarrier[3] = true; break;
                case 4: isCarrier[1] = true; isCarrier[3] = true; break;
                case 5: case 6: isCarrier[1] = isCarrier[2] = isCarrier[3] = true; break;
                case 7: isCarrier[0] = isCarrier[1] = isCarrier[2] = isCarrier[3] = true; break;
            }
            for (int op = 0; op < 4; op++)
                if (isCarrier[op]) p.op[op].TL = std::min(127, p.op[op].TL + tl_add);
            ym_->load_patch(p, ch);
        }
        double hz = IngameFMChip::midi_to_hz(v.pending_note);
        ym_->set_frequency(ch, hz, 0);
        ym_->key_on(ch);
        v.pending_has_note = false;
    }

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
            if (in_gap && v.sample_in_row >= KEY_OFF_GAP_SAMPLES)
                sfx_commit_keyon(ch);
            if (v.sample_in_row >= v.samples_per_row)
            {
                v.sample_in_row = 0;
                v.ticks_remaining--;
                if (v.ticks_remaining <= 0)
                {
                    ym_->key_off(ch);
                    v = SfxVoiceState{};
                    return;
                }
                v.current_row++;
                if (v.current_row >= static_cast<int>(def.song.rows.size()))
                    v.current_row = 0;
                sfx_process_row(ch, v.current_row);
            }
        }
    }

    static constexpr int KEY_OFF_GAP_SAMPLES = 44;

    void audio_callback(int16_t* stream, int samples)
    {
        if (finished_.load()) { std::memset(stream, 0, samples * 4); return; }
        int remaining = samples;
        int16_t* out  = stream;
        while (remaining > 0)
        {
            int pos_in_row    = sample_in_row_;
            bool in_gap       = pos_in_row < KEY_OFF_GAP_SAMPLES;
            int next_boundary = in_gap ? KEY_OFF_GAP_SAMPLES : samples_per_row_;
            int to_generate   = std::min(remaining, next_boundary - pos_in_row);

            // Single generate pass. Volume scalar: music_vol_ normally,
            // sfx_vol_ when any SFX voice is active. Applied as float multiply
            // on the output samples — no TL manipulation, timbre preserved.
            {
                ym_->generate(out, to_generate);
                float scalar = music_vol_.load();
                if (sfx_voices_ > 0) {
                    float sv = sfx_vol_.load();
                    for (int c = MAX_CHANNELS - sfx_voices_; c < MAX_CHANNELS; ++c)
                        if (sfx_voice_[c].active()) { scalar = sv; break; }
                }
                if (scalar < 1.0f) {
                    const int n = to_generate * 2;
                    for (int i = 0; i < n; ++i)
                        out[i] = static_cast<int16_t>(out[i] * scalar);
                }
            }

            for (int ch = MAX_CHANNELS - sfx_voices_; ch < MAX_CHANNELS; ++ch)
                sfx_tick_voice(ch, to_generate);

            out            += to_generate * 2;
            remaining      -= to_generate;
            sample_in_row_ += to_generate;

            if (in_gap && sample_in_row_ >= KEY_OFF_GAP_SAMPLES)
                commit_keyon();

            if (sample_in_row_ >= samples_per_row_)
            {
                sample_in_row_ = 0;
                current_row_++;
                if (current_row_ >= static_cast<int>(song_.rows.size()))
                {
                    if (loop_)
                    {
                        for (auto& p : pending_) p = {};
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
