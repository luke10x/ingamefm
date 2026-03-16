#pragma once
#include <sstream>
// =============================================================================
// ingamefm_player.h  — part of ingamefm (header-only)
//
// Two-chip architecture:
//
//   ym_music_  — runs the Furnace song on channels 0..MAX_CHANNELS-1.
//                Completely unaware of SFX. Never interrupted.
//                Output scaled by music_vol_.
//
//   ym_sfx_    — a voice pool of sfx_voices_ independent channels (0..N-1).
//                Channels on ym_sfx_ are completely separate from ym_music_.
//                SFX grab free/lowest-priority voices and play there.
//                Output scaled by sfx_vol_.
//                Only mixed into the output when at least one voice is active,
//                preventing idle-chip hum.
//
// The two chips are generated independently each buffer and summed in float.
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

struct IngameFMEvent  { int note; int instrument; int volume; };
struct IngameFMRow    { std::vector<IngameFMEvent> channels; };
struct IngameFMSong   { int num_rows=0; int num_channels=0; std::vector<IngameFMRow> rows; };

static std::string trim_right(const std::string& s)
{
    size_t end = s.find_last_not_of(" \t\r\n");
    return (end == std::string::npos) ? "" : s.substr(0, end + 1);
}

static int parse_note_field(const char* nc, int line_num)
{
    if (nc[0]=='.'&&nc[1]=='.'&&nc[2]=='.') return NOTE_NONE;
    if (nc[0]=='O'&&nc[1]=='F'&&nc[2]=='F') return NOTE_OFF;
    int semitone = -1;
    switch (nc[0]) {
        case 'C': semitone=0;  break; case 'D': semitone=2;  break;
        case 'E': semitone=4;  break; case 'F': semitone=5;  break;
        case 'G': semitone=7;  break; case 'A': semitone=9;  break;
        case 'B': semitone=11; break;
        default: throw std::runtime_error(std::string("Line ")+std::to_string(line_num)+": bad note '"+std::string(nc,3)+"'");
    }
    if      (nc[1]=='#') semitone++;
    else if (nc[1]=='-') {}
    else throw std::runtime_error(std::string("Line ")+std::to_string(line_num)+": bad accidental");
    if (nc[2]<'0'||nc[2]>'9') throw std::runtime_error(std::string("Line ")+std::to_string(line_num)+": bad octave");
    return 12 + (nc[2]-'0')*12 + semitone;
}

static int parse_hex2(const char* p, int line_num, const char* field_name)
{
    if (p[0]=='.'&&p[1]=='.') return -1;
    auto hex = [&](char c) -> int {
        if (c>='0'&&c<='9') return c-'0';
        if (c>='A'&&c<='F') return c-'A'+10;
        if (c>='a'&&c<='f') return c-'a'+10;
        throw std::runtime_error(std::string("Line ")+std::to_string(line_num)+": bad hex in "+field_name);
        return 0;
    };
    return hex(p[0])*16+hex(p[1]);
}

static IngameFMEvent parse_channel_column(const std::string& row, size_t colStart, size_t colWidth, int line_num)
{
    IngameFMEvent ev; ev.note=NOTE_NONE; ev.instrument=-1; ev.volume=-1;
    std::string col = row.substr(colStart, colWidth);
    while (col.size()<colWidth) col+='.';
    ev.note = parse_note_field(col.c_str(), line_num);
    if (col.size()>=5) ev.instrument = parse_hex2(col.c_str()+3, line_num, "instrument");
    if (col.size()>=7) ev.volume     = parse_hex2(col.c_str()+5, line_num, "volume");
    return ev;
}

static IngameFMSong parse_ingamefm_song(const std::string& text)
{
    std::vector<std::string> lines;
    { std::istringstream ss(text); std::string l; while(std::getline(ss,l)) lines.push_back(trim_right(l)); }
    while (!lines.empty()&&lines.front().empty()) lines.erase(lines.begin());
    if (lines.empty()) throw std::runtime_error("Song text is empty");
    int lineNum=1;
    if (lines[0].find("org.tildearrow.furnace")!=std::string::npos) { lines.erase(lines.begin()); lineNum++; }
    while (!lines.empty()&&lines.front().empty()) { lines.erase(lines.begin()); lineNum++; }
    if (lines.empty()) throw std::runtime_error("Song text has no content after header");
    int num_rows=0;
    { const std::string& rc=lines[0]; char* e=nullptr; long v=std::strtol(rc.c_str(),&e,10);
      while(e&&*e==' ')e++; if(!e||*e!='\0') throw std::runtime_error("Line "+std::to_string(lineNum)+": expected row count");
      if(v<=0||v>65536) throw std::runtime_error("Line "+std::to_string(lineNum)+": row count out of range");
      num_rows=static_cast<int>(v); lines.erase(lines.begin()); lineNum++; }
    IngameFMSong song; song.num_rows=num_rows;
    std::vector<std::string> dataLines;
    for (const auto& l:lines) if(!l.empty()) dataLines.push_back(l);
    if (dataLines.empty()) throw std::runtime_error("Song has row count but no data lines");
    auto split_channels=[](const std::string& row)->std::vector<std::string>{
        std::vector<std::string> cols; std::string cur;
        for(char c:row){ if(c=='|'){cols.push_back(cur);cur.clear();}else cur+=c; }
        if(!cur.empty()) cols.push_back(cur); return cols; };
    std::vector<std::string> firstCols=split_channels(dataLines[0]);
    song.num_channels=static_cast<int>(firstCols.size());
    std::vector<size_t> colWidths; for(const auto& c:firstCols) colWidths.push_back(c.size());
    for(int ch=0;ch<song.num_channels;ch++)
        if(colWidths[ch]<7) throw std::runtime_error("Channel "+std::to_string(ch)+" too narrow");
    int parsedRows=0;
    for(const auto& line:dataLines) {
        if(parsedRows>=num_rows) break;
        IngameFMRow row; std::vector<std::string> cols=split_channels(line);
        while(static_cast<int>(cols.size())<song.num_channels) cols.push_back("");
        for(int ch=0;ch<song.num_channels;ch++) {
            std::string col=cols[ch]; while(col.size()<colWidths[ch]) col+='.';
            row.channels.push_back(parse_channel_column(col,0,colWidths[ch],lineNum)); }
        song.rows.push_back(row); parsedRows++; lineNum++; }
    if(parsedRows<num_rows) throw std::runtime_error("Song declared "+std::to_string(num_rows)+" rows but found "+std::to_string(parsedRows));
    return song;
}

// ─────────────────────────────────────────────────────────────────────────────
// Channel state (music player)
// ─────────────────────────────────────────────────────────────────────────────

struct IngameFMChannelState { bool active=false; int instrument=0; int volume=0x7F; };

// ─────────────────────────────────────────────────────────────────────────────
// SFX voice state — one per voice slot on ym_sfx_
// ─────────────────────────────────────────────────────────────────────────────

struct SfxVoiceState {
    int  sfx_id=-1; int priority=0; int ticks_remaining=0;
    int  current_row=0; int sample_in_row=0; int samples_per_row=0;
    bool pending_has_note=false; bool pending_is_off=false;
    int  pending_note=0; int pending_inst=0; int pending_vol=0x7F;
    int  last_instrument=0; int last_volume=0x7F;
    bool active() const { return sfx_id>=0 && ticks_remaining>0; }
};

// ─────────────────────────────────────────────────────────────────────────────
// Song-change timing
// ─────────────────────────────────────────────────────────────────────────────

enum class SongChangeWhen {
    NOW,            // Switch immediately on the next audio buffer
    AT_PATTERN_END  // Wait until the current song reaches its last row, then switch
};

// ─────────────────────────────────────────────────────────────────────────────
// IngameFMPlayer
// ─────────────────────────────────────────────────────────────────────────────

class IngameFMPlayer
{
public:
    // Maximum channels for the music player (all on ym_music_)
    static constexpr int MAX_CHANNELS  = 6;
    // Maximum voices available for SFX (all on ym_sfx_)
    static constexpr int MAX_SFX_VOICES = 6;

    // ── Volume ───────────────────────────────────────────────────────────────
    // 0.0 = silent, 1.0 = full. Thread-safe, no lock needed.
    void set_music_volume(float v) { music_vol_.store(std::max(0.f,std::min(1.f,v))); }
    void set_sfx_volume  (float v) { sfx_vol_  .store(std::max(0.f,std::min(1.f,v))); }

    // ── Song setup ───────────────────────────────────────────────────────────
    void set_song(const std::string& text, int tick_rate, int speed)
    {
        if(tick_rate<=0) throw std::runtime_error("tick_rate must be > 0");
        if(speed<=0)     throw std::runtime_error("speed must be > 0");
        song_=parse_ingamefm_song(text); tick_rate_=tick_rate; speed_=speed;
        samples_per_row_=static_cast<int>(static_cast<double>(SAMPLE_RATE)/tick_rate_*speed_);
    }

    void add_patch(int instrument_id, const YM2612Patch& patch)
    {
        if(instrument_id<0||instrument_id>255) throw std::runtime_error("instrument_id must be 0-255");
        patches_[instrument_id]=patch; patches_present_[instrument_id]=true;
    }

    // ── Song change (call with audio device locked or before start) ─────────────
    // Queues a song change. The new song is pre-parsed from text immediately
    // (throws on parse error). Actual switch happens according to `when`:
    //   NOW           — at the start of the next audio buffer
    //   AT_PATTERN_END — after the current song finishes its last row
    // `start_row` is clamped to the new song's row count.
    void change_song(const std::string& text, int tick_rate, int speed,
                     SongChangeWhen when = SongChangeWhen::AT_PATTERN_END,
                     int start_row = 0)
    {
        if(tick_rate<=0) throw std::runtime_error("tick_rate must be > 0");
        if(speed<=0)     throw std::runtime_error("speed must be > 0");
        PendingSong ps;
        ps.song            = parse_ingamefm_song(text);
        ps.tick_rate       = tick_rate;
        ps.speed           = speed;
        ps.samples_per_row = static_cast<int>(
            static_cast<double>(SAMPLE_RATE)/tick_rate*speed);
        ps.start_row       = std::max(0, std::min(start_row,
                                 static_cast<int>(ps.song.rows.size())-1));
        ps.when            = when;
        ps.pending         = true;
        pending_song_      = std::move(ps);
    }

    // Returns the current playback row. Thread-safe (atomic read).
    int get_current_row()  const { return current_row_.load(); }

    // Returns the total number of rows in the currently playing song.
    // Call with audio device locked (reads song_ without synchronisation).
    int get_song_length()  const { return static_cast<int>(song_.rows.size()); }

    // ── SFX setup ────────────────────────────────────────────────────────────
    // sfx_set_voices(n): how many voices ym_sfx_ exposes for SFX (1..MAX_SFX_VOICES).
    // Default 3. Call before start().
    void sfx_set_voices(int n)
    {
        if(n<1||n>MAX_SFX_VOICES) throw std::runtime_error("sfx_set_voices: n must be 1.."+std::to_string(MAX_SFX_VOICES));
        sfx_voices_=n;
    }

    // sfx_define: pre-parse an SFX pattern. May be called at any time.
    void sfx_define(int id, const std::string& pattern, int tick_rate, int speed)
    {
        if(id<0||id>255) throw std::runtime_error("sfx_define: id must be 0-255");
        if(tick_rate<=0||speed<=0) throw std::runtime_error("sfx_define: tick_rate and speed must be > 0");
        SfxDef def; def.song=parse_ingamefm_song(pattern); def.tick_rate=tick_rate; def.speed=speed;
        def.samples_per_row=static_cast<int>(static_cast<double>(SAMPLE_RATE)/tick_rate*speed);
        sfx_defs_[id]=std::move(def); sfx_defs_present_[id]=true;
    }

    // ── Playback ─────────────────────────────────────────────────────────────
    void play(bool loop=false)
    {
        if(song_.rows.empty()) throw std::runtime_error("No song loaded");
        reset_state(loop);
        SDL_AudioSpec desired{}; desired.freq=SAMPLE_RATE; desired.format=AUDIO_S16SYS;
        desired.channels=2; desired.samples=512; desired.callback=s_audio_callback; desired.userdata=this;
        SDL_AudioSpec obtained{};
        SDL_AudioDeviceID dev=SDL_OpenAudioDevice(nullptr,0,&desired,&obtained,0);
        if(dev==0) throw std::runtime_error(std::string("SDL_OpenAudioDevice failed: ")+SDL_GetError());
        SDL_PauseAudioDevice(dev,0);
        while(!finished_.load()) SDL_Delay(10);
        SDL_CloseAudioDevice(dev);
    }

    void start(SDL_AudioDeviceID dev, bool loop=false)
    {
        if(song_.rows.empty()) throw std::runtime_error("No song loaded");
        SDL_LockAudioDevice(dev); reset_state(loop); SDL_UnlockAudioDevice(dev);
    }

    void stop(SDL_AudioDeviceID dev)
    {
        SDL_LockAudioDevice(dev);
        finished_.store(true);
        for(int ch=0;ch<MAX_CHANNELS;ch++) if(ym_music_) ym_music_->key_off(ch);
        for(int v=0;v<sfx_voices_;v++)     if(ym_sfx_)   ym_sfx_->key_off(v);
        SDL_UnlockAudioDevice(dev);
    }

    bool is_finished() const { return finished_.load(); }

    // Direct chip access for manual note triggers outside the song/SFX systems.
    // Always lock the SDL audio device first.
    IngameFMChip* chip() { return ym_music_.get(); }

    // ── SFX trigger ──────────────────────────────────────────────────────────
    // Call with the SDL audio device locked.
    // Scans ym_sfx_ voice pool right-to-left: picks free voice first,
    // then lowest-priority active voice if all busy.
    void sfx_play(int id, int priority, int duration_ticks)
    {
        if(!sfx_defs_present_[id]) return;
        if(sfx_voices_==0)         return;
        if(!ym_sfx_)               return;

        const SfxDef& def = sfx_defs_[id];
        int best_v = -1, best_prio = priority;

        for(int v=sfx_voices_-1; v>=0; --v) {
            if(!sfx_voice_[v].active()) { best_v=v; best_prio=-1; break; }
            if(sfx_voice_[v].priority < best_prio) { best_v=v; best_prio=sfx_voice_[v].priority; }
        }
        if(best_v<0) return;

        // Silence whatever was on this voice
        ym_sfx_->key_off(best_v);

        // Set up voice state
        SfxVoiceState& vs = sfx_voice_[best_v];
        vs.sfx_id          = id;
        vs.priority        = priority;
        vs.ticks_remaining = duration_ticks;
        vs.current_row     = 0;
        vs.sample_in_row   = 0;
        vs.samples_per_row = def.samples_per_row;
        vs.pending_has_note= false;
        vs.pending_is_off  = false;
        vs.last_instrument = 0;
        vs.last_volume     = 0x7F;

        // Fire row 0 immediately (skip_keyoff: eviction already did it)
        sfx_process_row(best_v, 0, true);
        sfx_commit_keyon(best_v);
        vs.sample_in_row = KEY_OFF_GAP_SAMPLES;
    }

    static void s_audio_callback(void* userdata, Uint8* stream, int len)
    {
        static_cast<IngameFMPlayer*>(userdata)->audio_callback(
            reinterpret_cast<int16_t*>(stream), len/4);
    }

    static constexpr int SAMPLE_RATE = 44100;

private:
    // ── SFX definitions ──────────────────────────────────────────────────────
    struct SfxDef { IngameFMSong song; int tick_rate=60; int speed=6; int samples_per_row=0; };
    std::array<SfxDef, 256>         sfx_defs_{};
    std::array<bool,   256>         sfx_defs_present_{};
    int                             sfx_voices_ = 3;
    std::array<SfxVoiceState, MAX_SFX_VOICES> sfx_voice_{};

    // ── Music state ──────────────────────────────────────────────────────────
    IngameFMSong song_;
    int  tick_rate_=60, speed_=6, samples_per_row_=0;
    std::atomic<int> current_row_{0};
    int  sample_in_row_=0;
    bool loop_=false;
    std::atomic<bool> finished_{false};
    std::array<IngameFMChannelState, MAX_CHANNELS> ch_state_{};
    std::array<YM2612Patch, 256>  patches_{};
    std::array<bool, 256>         patches_present_{};

    struct PendingNote { bool has_note=false; bool is_off=false; int midi_note=0; int instId=0; int volume=0x7F; };
    std::array<PendingNote, MAX_CHANNELS> pending_{};

    // ── Pending song change ──────────────────────────────────────────────────
    struct PendingSong {
        IngameFMSong   song;
        int            tick_rate       = 60;
        int            speed           = 6;
        int            samples_per_row = 0;
        int            start_row       = 0;
        SongChangeWhen when            = SongChangeWhen::AT_PATTERN_END;
        bool           pending         = false;
    };
    PendingSong pending_song_{};

    // ── Chips ────────────────────────────────────────────────────────────────
    std::unique_ptr<IngameFMChip> ym_music_;
    std::unique_ptr<IngameFMChip> ym_sfx_;
    std::atomic<float> music_vol_{1.0f};
    std::atomic<float> sfx_vol_  {1.0f};

    static constexpr int KEY_OFF_GAP_SAMPLES = 44;

    // ── Init ─────────────────────────────────────────────────────────────────
    void reset_state(bool loop)
    {
        loop_=loop; current_row_.store(0); sample_in_row_=0; finished_.store(false); pending_song_.pending=false;
        for(auto& c:ch_state_) c=IngameFMChannelState{};
        for(auto& p:pending_)  p={};
        for(auto& v:sfx_voice_) v=SfxVoiceState{};
        ym_music_=std::make_unique<IngameFMChip>();
        ym_sfx_  =std::make_unique<IngameFMChip>();
        // ymfm defaults 0xB4 to 0 (both channels muted). Enable L+R everywhere.
        for(int c=0;c<3;++c) {
            ym_music_->write(0,0xB4+c,0xC0); ym_music_->write(1,0xB4+c,0xC0);
            ym_sfx_  ->write(0,0xB4+c,0xC0); ym_sfx_  ->write(1,0xB4+c,0xC0);
        }
        process_row(0);
        commit_keyon();
        sample_in_row_=KEY_OFF_GAP_SAMPLES;
    }

    // ── Music player helpers ──────────────────────────────────────────────────
    // Helper: scale carrier TLs by per-note volume, return modified patch copy.
    static YM2612Patch apply_volume(const YM2612Patch& src, int vol)
    {
        YM2612Patch p=src;
        int tl_add=((0x7F-vol)*127)/0x7F;
        bool isCarrier[4]={false,false,false,false};
        switch(p.ALG) {
            case 0:case 1:case 2:case 3: isCarrier[3]=true; break;
            case 4: isCarrier[1]=isCarrier[3]=true; break;
            case 5:case 6: isCarrier[1]=isCarrier[2]=isCarrier[3]=true; break;
            case 7: isCarrier[0]=isCarrier[1]=isCarrier[2]=isCarrier[3]=true; break;
        }
        for(int op=0;op<4;op++) if(isCarrier[op]) p.op[op].TL=std::min(127,p.op[op].TL+tl_add);
        return p;
    }

    // Apply a pending song change immediately.
    // Silences all music channels, swaps song state, restarts from start_row.
    // Must be called from the audio callback (already under audio lock).
    void apply_pending_song()
    {
        PendingSong& ps = pending_song_;
        // Silence all music channels cleanly
        for(int ch=0;ch<MAX_CHANNELS;ch++) ym_music_->key_off(ch);
        for(auto& c:ch_state_) c=IngameFMChannelState{};
        for(auto& p:pending_)  p={};
        // Swap in new song
        song_            = std::move(ps.song);
        tick_rate_       = ps.tick_rate;
        speed_           = ps.speed;
        samples_per_row_ = ps.samples_per_row;
        current_row_.store(ps.start_row);
        sample_in_row_   = 0;
        finished_.store(false);
        ps.pending       = false;
        // Fire first row immediately (with gap so key_on lands cleanly)
        process_row(current_row_.load());
        commit_keyon();
        sample_in_row_   = KEY_OFF_GAP_SAMPLES;
    }

    void process_row(int rowIdx)
    {
        if(rowIdx>=(int)song_.rows.size()) return;
        const IngameFMRow& row=song_.rows[rowIdx];
        int numCh=std::min((int)row.channels.size(),MAX_CHANNELS);
        for(int ch=0;ch<numCh;ch++) {
            const IngameFMEvent& ev=row.channels[ch];
            pending_[ch]={};
            if(ev.instrument>=0) ch_state_[ch].instrument=ev.instrument;
            if(ev.volume>=0)     ch_state_[ch].volume=ev.volume;
            if(ev.note==NOTE_OFF) {
                ym_music_->key_off(ch); ch_state_[ch].active=false; pending_[ch].is_off=true;
            } else if(ev.note>=0) {
                ym_music_->key_off(ch); ch_state_[ch].active=false;
                pending_[ch].has_note=true; pending_[ch].midi_note=ev.note;
                pending_[ch].instId=ch_state_[ch].instrument; pending_[ch].volume=ch_state_[ch].volume;
            }
        }
    }

    void commit_keyon()
    {
        for(int ch=0;ch<MAX_CHANNELS;ch++) {
            if(!pending_[ch].has_note) continue;
            int instId=pending_[ch].instId;
            if(patches_present_[instId]) {
                YM2612Patch p=apply_volume(patches_[instId],pending_[ch].volume);
                ym_music_->load_patch(p,ch);
            }
            ym_music_->set_frequency(ch,IngameFMChip::midi_to_hz(pending_[ch].midi_note),0);
            ym_music_->key_on(ch);
            ch_state_[ch].active=true; pending_[ch].has_note=false;
        }
    }

    // ── SFX voice pool helpers ────────────────────────────────────────────────
    // Voice index v is a slot on ym_sfx_ (0..sfx_voices_-1), NOT a music channel.

    void sfx_process_row(int v, int rowIdx, bool skip_keyoff=false)
    {
        SfxVoiceState& vs=sfx_voice_[v];
        const SfxDef& def=sfx_defs_[vs.sfx_id];
        vs.pending_has_note=false; vs.pending_is_off=false;
        if(rowIdx>=(int)def.song.rows.size()) return;
        const IngameFMEvent& ev=def.song.rows[rowIdx].channels[0];
        if(ev.instrument>=0) vs.last_instrument=ev.instrument;
        if(ev.volume>=0)     vs.last_volume=ev.volume;
        if(ev.note==NOTE_OFF)  { if(!skip_keyoff) ym_sfx_->key_off(v); vs.pending_is_off=true; }
        else if(ev.note>=0)    {
            if(!skip_keyoff) ym_sfx_->key_off(v);
            vs.pending_has_note=true; vs.pending_note=ev.note;
            vs.pending_inst=vs.last_instrument; vs.pending_vol=vs.last_volume;
        }
    }

    void sfx_commit_keyon(int v)
    {
        SfxVoiceState& vs=sfx_voice_[v];
        if(!vs.pending_has_note) return;
        if(patches_present_[vs.pending_inst]) {
            YM2612Patch p=apply_volume(patches_[vs.pending_inst],vs.pending_vol);
            ym_sfx_->load_patch(p,v);
        }
        ym_sfx_->set_frequency(v,IngameFMChip::midi_to_hz(vs.pending_note),0);
        ym_sfx_->key_on(v);
        vs.pending_has_note=false;
    }

    void sfx_tick_voice(int v, int samples)
    {
        SfxVoiceState& vs=sfx_voice_[v];
        if(!vs.active()) return;
        const SfxDef& def=sfx_defs_[vs.sfx_id];
        int remaining=samples;
        while(remaining>0) {
            bool in_gap=vs.sample_in_row<KEY_OFF_GAP_SAMPLES;
            int next_bound=in_gap?KEY_OFF_GAP_SAMPLES:vs.samples_per_row;
            int to_advance=std::min(remaining,next_bound-vs.sample_in_row);
            vs.sample_in_row+=to_advance; remaining-=to_advance;
            if(in_gap&&vs.sample_in_row>=KEY_OFF_GAP_SAMPLES) sfx_commit_keyon(v);
            if(vs.sample_in_row>=vs.samples_per_row) {
                vs.sample_in_row=0; vs.ticks_remaining--;
                if(vs.ticks_remaining<=0) { ym_sfx_->key_off(v); vs=SfxVoiceState{}; return; }
                vs.current_row++;
                if(vs.current_row>=(int)def.song.rows.size()) vs.current_row=0;
                sfx_process_row(v,vs.current_row);
            }
        }
    }

    // ── Audio callback ────────────────────────────────────────────────────────

    void audio_callback(int16_t* stream, int samples)
    {
        if(finished_.load()) { std::memset(stream,0,samples*4); return; }

        // Check for an immediate song change before generating anything
        if(pending_song_.pending && pending_song_.when==SongChangeWhen::NOW)
            apply_pending_song();

        int remaining=samples;
        int16_t* out=stream;

        while(remaining>0) {
            int pos_in_row=sample_in_row_;
            bool in_gap=pos_in_row<KEY_OFF_GAP_SAMPLES;
            int next_boundary=in_gap?KEY_OFF_GAP_SAMPLES:samples_per_row_;
            int to_generate=std::min(remaining,next_boundary-pos_in_row);

            // ── Generate ──────────────────────────────────────────────────────
            const float mv=music_vol_.load();
            const float sv=sfx_vol_.load();
            const int   n =to_generate*2;

            ym_music_->generate(out, to_generate);
            if(mv<1.0f)
                for(int i=0;i<n;++i) out[i]=static_cast<int16_t>(out[i]*mv);

            // Only mix SFX chip when at least one voice is active (prevents hum)
            bool any_sfx=false;
            for(int v=0;v<sfx_voices_;++v) if(sfx_voice_[v].active()){any_sfx=true;break;}

            int16_t sfx_buf[512*2];  // max SDL buffer = 512 frames
            ym_sfx_->generate(sfx_buf, to_generate);  // always advance clock

            if(any_sfx && sv>0.0f) {
                for(int i=0;i<n;++i) {
                    float mixed=static_cast<float>(out[i])
                               +static_cast<float>(sfx_buf[i])*sv;
                    out[i]=static_cast<int16_t>(std::max(-32768.f,std::min(32767.f,mixed)));
                }
            }

            // ── Tick SFX voices ───────────────────────────────────────────────
            for(int v=0;v<sfx_voices_;++v) sfx_tick_voice(v,to_generate);

            out+=to_generate*2; remaining-=to_generate; sample_in_row_+=to_generate;

            if(in_gap&&sample_in_row_>=KEY_OFF_GAP_SAMPLES) commit_keyon();

            if(sample_in_row_>=samples_per_row_) {
                sample_in_row_=0;
                int row=current_row_.load()+1;
                if(row>=(int)song_.rows.size()) {
                    // Pattern end — apply queued AT_PATTERN_END song change if any
                    if(pending_song_.pending &&
                       pending_song_.when==SongChangeWhen::AT_PATTERN_END) {
                        apply_pending_song();
                        // apply_pending_song already called process_row + commit_keyon
                        // and set sample_in_row_ = KEY_OFF_GAP_SAMPLES, so skip below
                        break;
                    }
                    if(loop_) { for(auto& p:pending_) p={}; row=0; }
                    else {
                        std::memset(out,0,remaining*4);
                        for(int ch=0;ch<MAX_CHANNELS;ch++) ym_music_->key_off(ch);
                        finished_.store(true); return;
                    }
                }
                current_row_.store(row);
                process_row(row);
            }
        }
    }
};
