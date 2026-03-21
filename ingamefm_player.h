#pragma once
// =============================================================================
// ingamefm_player.h — Configurable Sample Rate Version
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
#include <unordered_map>
#include <memory>

static constexpr int NOTE_NONE = -1;
static constexpr int NOTE_OFF  = -2;

struct IngameFMEvent  { int note; int instrument; int volume; };
struct IngameFMRow    { std::vector<IngameFMEvent> channels; };
struct IngameFMSong   { int num_rows=0; int num_channels=0; std::vector<IngameFMRow> rows; };

static std::string trim_right(const std::string& s) {
    size_t end = s.find_last_not_of(" \t\r\n");
    return (end == std::string::npos) ? "" : s.substr(0, end + 1);
}

static int parse_note_field(const char* nc, int line_num) {
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
    // Furnace octave numbering: C-4 = middle C = 261.6 Hz = MIDI 60.
    return 12 + (nc[2]-'0')*12 + semitone;
}

static int parse_hex2(const char* p, int line_num, const char* field_name) {
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

static IngameFMEvent parse_channel_column(const std::string& row, size_t colStart, size_t colWidth, int line_num) {
    IngameFMEvent ev; ev.note=NOTE_NONE; ev.instrument=-1; ev.volume=-1;
    std::string col = row.substr(colStart, colWidth);
    while (col.size()<colWidth) col+='.';
    ev.note = parse_note_field(col.c_str(), line_num);
    if (col.size()>=5) ev.instrument = parse_hex2(col.c_str()+3, line_num, "instrument");
    if (col.size()>=7) ev.volume     = parse_hex2(col.c_str()+5, line_num, "volume");
    return ev;
}

static IngameFMSong parse_ingamefm_song(const std::string& text) {
    std::vector<std::string> lines;
    { std::string l; for(char c:text){if(c=='\n'){lines.push_back(trim_right(l));l.clear();}else if(c!='\r')l+=c;} if(!l.empty())lines.push_back(trim_right(l)); }
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
        song.rows.push_back(row); parsedRows++; lineNum++;
    }
    if(parsedRows<num_rows) throw std::runtime_error("Song declared "+std::to_string(num_rows)+" rows but found "+std::to_string(parsedRows));
    return song;
}

struct IngameFMChannelState { bool active=false; int instrument=0; int volume=0x7F; };

struct SfxVoiceState {
    int  sfx_id=-1; int priority=0; int ticks_remaining=0;
    int  current_row=0; int sample_in_row=0; int samples_per_row=0;
    bool pending_has_note=false; bool pending_is_off=false;
    int  pending_note=0; int pending_inst=0; int pending_vol=0x7F;
    int  last_instrument=0; int last_volume=0x7F;
    int  cache_key_id = -1;
    bool active() const { return sfx_id>=0 && ticks_remaining>0; }
};

enum class SongChangeWhen { NOW, AT_PATTERN_END };

class IngameFMPlayer
{
public:
    static constexpr int MAX_CHANNELS   = 6;
    static constexpr int MAX_SFX_VOICES = 6;

    // ── Configuration ────────────────────────────────────────────────────────
    // Set sample rate BEFORE defining songs/SFX. Default 44100.
    void set_sample_rate(int rate) {
        if(rate <= 0) throw std::runtime_error("Sample rate must be > 0");
        sample_rate_ = rate;
    }
    int get_sample_rate() const { return sample_rate_; }

    // Set chip type — call before start(), requires teardown+reinit to take effect.
    void set_chip_type(IngameFMChipType t) {
        chip_type_ = t;
        if(ym_music_) ym_music_->set_chip_type(t);
        if(ym_sfx_)   ym_sfx_->set_chip_type(t);
    }
    IngameFMChipType get_chip_type() const { return chip_type_; }

    void set_music_volume(float v) { music_vol_.store(std::max(0.f,std::min(1.f,v))); }
    void set_sfx_volume  (float v) { sfx_vol_  .store(std::max(0.f,std::min(1.f,v))); }

    void add_patch(int instrument_id, const YM2612Patch& patch,
                   int block=0, int lfo_enable=0, int lfo_freq=0) {
        if(instrument_id<0||instrument_id>255) throw std::runtime_error("instrument_id must be 0-255");
        patches_[instrument_id] = {patch, block, lfo_enable, lfo_freq};
        patches_present_[instrument_id]=true;
    }

    // Global chip LFO — written directly to register 0x22 of each chip.
    // Overrides per-patch lfo settings. Call any time (locks not needed if
    // called from audio callback, or lock device before calling from main thread).
    void set_music_lfo(bool enable, int freq=0) {
        music_lfo_enable_ = enable;
        music_lfo_freq_   = freq & 7;
        if(ym_music_) ym_music_->enable_lfo(enable, (uint8_t)music_lfo_freq_);
    }
    void set_sfx_lfo(bool enable, int freq=0) {
        sfx_lfo_enable_ = enable;
        sfx_lfo_freq_   = freq & 7;
        if(ym_sfx_) ym_sfx_->enable_lfo(enable, (uint8_t)sfx_lfo_freq_);
    }
    bool get_music_lfo_enable() const { return music_lfo_enable_; }
    int  get_music_lfo_freq()   const { return music_lfo_freq_; }
    bool get_sfx_lfo_enable()   const { return sfx_lfo_enable_; }
    int  get_sfx_lfo_freq()     const { return sfx_lfo_freq_; }

    // ── Song definition ──────────────────────────────────────────────────────
    void song_define(int id, const std::string& text, int tick_rate, int speed) {
        if(tick_rate<=0) throw std::runtime_error("tick_rate must be > 0");
        if(speed<=0)     throw std::runtime_error("speed must be > 0");
        DefinedSong def;
        def.song            = parse_ingamefm_song(text);
        def.tick_rate       = tick_rate;
        def.speed           = speed;
        def.samples_per_row = static_cast<int>(static_cast<double>(sample_rate_)/tick_rate*speed);
        def.cache_key_id    = id;
        def.valid           = true;
        if((build_cache_||use_cache_) && song_cache_.find(id)==song_cache_.end())
            song_cache_[id] = prerender_song(def.song, tick_rate, speed, patches_, patches_present_, sample_rate_);
        defined_songs_[id]  = std::move(def);
    }

    // ── SFX definition ───────────────────────────────────────────────────────
    void sfx_define(int id, const std::string& pattern, int tick_rate, int speed) {
        if(id<0||id>255) throw std::runtime_error("sfx_define: id must be 0-255");
        if(tick_rate<=0||speed<=0) throw std::runtime_error("sfx_define: tick_rate and speed must be > 0");
        SfxDef def;
        def.song            = parse_ingamefm_song(pattern);
        def.tick_rate       = tick_rate;
        def.speed           = speed;
        def.samples_per_row = static_cast<int>(static_cast<double>(sample_rate_)/tick_rate*speed);
        def.cache_key_id    = id;
        if((build_cache_||use_cache_) && sfx_cache_.find(id)==sfx_cache_.end())
            sfx_cache_[id]  = prerender_sfx(def.song, tick_rate, speed, patches_, patches_present_, sample_rate_);
        sfx_defs_[id]       = std::move(def);
        sfx_defs_present_[id] = true;
    }

    // ── Song selection ───────────────────────────────────────────────────────
    void song_select(int id, bool loop=false) {
        auto it = defined_songs_.find(id);
        if(it==defined_songs_.end())
            throw std::runtime_error("song_select: ID "+std::to_string(id)+" not defined.");
        const DefinedSong& def = it->second;
        ensure_chips();
        for(int ch=0;ch<MAX_CHANNELS;ch++) ym_music_->key_off(ch);
        song_             = def.song;
        tick_rate_        = def.tick_rate;
        speed_            = def.speed;
        samples_per_row_  = def.samples_per_row;
        current_song_id_  = id;
        loop_             = loop;
        current_row_.store(0);
        sample_in_row_    = 0;
        finished_.store(false);
        pending_song_.pending = false;

        for(auto& c:ch_state_) c=IngameFMChannelState{};
        for(auto& p:pending_)  p={};
        if(!(play_cache_||use_cache_)) {
            process_row(0);
            commit_keyon();
            sample_in_row_ = KEY_OFF_GAP_SAMPLES;
        } else {
            sample_in_row_ = 0;
        }
    }

    // ── Song change (call with audio device locked) ───────────────────────────
    void change_song(int id,
                     SongChangeWhen when = SongChangeWhen::AT_PATTERN_END,
                     int start_row = 0) {
        auto it = defined_songs_.find(id);
        if(it==defined_songs_.end())
            throw std::runtime_error("change_song: ID "+std::to_string(id)+" not defined.");
        const DefinedSong& def = it->second;
        PendingSong ps;
        ps.song            = def.song;
        ps.tick_rate       = def.tick_rate;
        ps.speed           = def.speed;
        ps.samples_per_row = def.samples_per_row;
        ps.start_row       = std::max(0, std::min(start_row, (int)ps.song.rows.size()-1));
        ps.when            = when;
        ps.pending         = true;
        ps.song_id         = id;
        pending_song_      = std::move(ps);
    }

    int get_current_row()  const { return current_row_.load(); }
    int get_song_length()  const { return (int)song_.rows.size(); }
    int get_current_song_id() const { return current_song_id_; }

    // ── SFX pool ─────────────────────────────────────────────────────────────
    void sfx_set_voices(int n) {
        if(n<1||n>MAX_SFX_VOICES) throw std::runtime_error("sfx_set_voices: n must be 1.."+std::to_string(MAX_SFX_VOICES));
        sfx_voices_=n;
    }

    // ── Playback ─────────────────────────────────────────────────────────────
    void start(SDL_AudioDeviceID dev, bool loop=false) {
        if(song_.rows.empty()) throw std::runtime_error("No song loaded — call song_select() first");
        SDL_LockAudioDevice(dev);
        ensure_chips();
        loop_=loop; current_row_.store(0); sample_in_row_=0;
        finished_.store(false); pending_song_.pending=false;
        for(auto& c:ch_state_) c=IngameFMChannelState{};
        for(auto& p:pending_)  p={};
        for(auto& v:sfx_voice_) v=SfxVoiceState{};
        init_panning();
        if(!(play_cache_||use_cache_)) {
            process_row(0); commit_keyon();
            sample_in_row_=KEY_OFF_GAP_SAMPLES;
        } else {
            sample_in_row_=0;
        }
        std::printf("[IngameFM] start: sample_rate=%d  spr=%d  rows=%d  build=%d play=%d\n",
                    sample_rate_, samples_per_row_, (int)song_.rows.size(), (int)build_cache_, (int)play_cache_);
        if(song_cache_.count(current_song_id_)) {
            const auto& c=song_cache_.at(current_song_id_);
            std::printf("[IngameFM] cache: spr=%d  rows=%d  samples=%d\n",
                        c.samples_per_row, c.total_rows, (int)c.samples.size());
        }
        SDL_UnlockAudioDevice(dev);
    }

    void stop(SDL_AudioDeviceID dev) {
        SDL_LockAudioDevice(dev);
        finished_.store(true);
        if(ym_music_) for(int ch=0;ch<MAX_CHANNELS;ch++) ym_music_->key_off(ch);
        if(ym_sfx_)   for(int v=0;v<sfx_voices_;v++)     ym_sfx_->key_off(v);
        SDL_UnlockAudioDevice(dev);
    }

    bool is_finished() const { return finished_.load(); }
    IngameFMChip* chip() { return ym_music_.get(); }

    // ── SFX trigger (call with audio device locked) ───────────────────────────
    void sfx_play(int id, int priority, int duration_ticks) {
        if(!sfx_defs_present_[id]) return;
        if(sfx_voices_==0||!ym_sfx_) return;
        const SfxDef& def = sfx_defs_[id];

        // In capture mode: allocate capture buffer with extra tail rows for release envelope,
        // and force duration to cover all rows + tail
        bool sfx_capturing = capture_session_active_;
        if(sfx_capturing && sfx_cache_.find(id)==sfx_cache_.end()) {
            static constexpr int TAIL_ROWS = 4;  // extra rows to capture release tail
            int total_rows = def.song.num_rows + TAIL_ROWS;
            CachedAudio c;
            c.samples_per_row = def.samples_per_row;
            c.total_rows      = total_rows;
            c.samples.assign(total_rows * def.samples_per_row * 2, 0);
            c.valid = true;
            sfx_cache_[id] = std::move(c);
            capture_sfx_rows_done_[id] = 0;
            std::printf("[IngameFM] SFX %d capture buffer: %d rows (%d pattern + %d tail) x %d spr\n",
                        id, total_rows, def.song.num_rows, TAIL_ROWS, def.samples_per_row);
            // duration covers pattern rows + tail so voice keeps generating release
            duration_ticks = total_rows;
        } else {
            std::printf("[IngameFM] SFX %d sfx_play: session=%d cache_exists=%d\n",
                        id, (int)capture_session_active_,
                        (int)(sfx_cache_.find(id)!=sfx_cache_.end()));
        }
        int best_v=-1, best_prio=priority;
        for(int v=sfx_voices_-1;v>=0;--v) {
            if(!sfx_voice_[v].active()) { best_v=v; best_prio=-1; break; }
            if(sfx_voice_[v].priority<best_prio) { best_v=v; best_prio=sfx_voice_[v].priority; }
        }
        if(best_v<0) return;
        ym_sfx_->key_off(best_v);
        SfxVoiceState& vs  = sfx_voice_[best_v];
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
        vs.cache_key_id    = def.cache_key_id;
        sfx_process_row(best_v, 0, true);
        if(capture_session_active_) {
            // Capture mode: same behavior as live — key-on commits after gap.
            // The gap samples (0..KEY_OFF_GAP_SAMPLES-1) capture the previous note's
            // release tail, which is then played back identically in cached mode.
            sfx_commit_keyon(best_v);
            vs.sample_in_row = KEY_OFF_GAP_SAMPLES;
        } else if(play_cache_||use_cache_) {
            // Cache playback: start from KEY_OFF_GAP_SAMPLES to match how it was recorded
            vs.sample_in_row = KEY_OFF_GAP_SAMPLES;
        } else {
            sfx_commit_keyon(best_v);
            vs.sample_in_row = KEY_OFF_GAP_SAMPLES;
        }
    }

    static void s_audio_callback(void* userdata, Uint8* stream, int len) {
        static_cast<IngameFMPlayer*>(userdata)->audio_callback(
            reinterpret_cast<int16_t*>(stream), len/4);
    }

    // ── Cache control ─────────────────────────────────────────────────────────
    // build_cache_: prerender audio into memory when song_define/sfx_define
    //   is called. Required for song_dump/sfx_dump to work.
    //   Does NOT affect live playback — chip still renders in real time.
    // play_cache_: read audio from prerendered buffer during playback
    //   instead of the live chip. Requires build_cache_ or xxd load.
    bool build_cache_ = false;
    bool play_cache_  = false;
    void set_build_cache(bool v) { build_cache_ = v; }
    void set_play_cache (bool v) { play_cache_  = v; }
    // Convenience: set_cache(true) enables both build and play.
    void set_cache(bool v)       { build_cache_ = v; play_cache_ = v; }

    // Live capture API.
    // request_capture(): schedules capture to begin at the next loop boundary (row 0).
    //   Sets capture_pending_=true. The callback will flip to capture_mode_ at row 0.
    //   capture_session_active_ stays true for the whole session until cancel/complete.
    //   Call while audio device is locked.
    void request_capture() {
        capture_sfx_rows_done_.clear();
        capture_song_rows_done_.store(0);
        capture_song_done_      = false;
        capture_pending_        = true;
        capture_mode_           = false;
        capture_session_active_ = true;
        // One SFX voice during capture so output maps cleanly to one buffer
        sfx_voices_captured_    = sfx_voices_;
        sfx_voices_             = 1;
        // Erase old caches so they get freshly allocated at loop start
        if(current_song_id_ >= 0) song_cache_.erase(current_song_id_);
        sfx_cache_.clear();
    }
    void cancel_capture() {
        capture_pending_        = false;
        capture_mode_           = false;
        capture_session_active_ = false;
        // Restore original sfx voice count
        if(sfx_voices_captured_ > 0) {
            sfx_voices_          = sfx_voices_captured_;
            sfx_voices_captured_ = 0;
        }
    }
    bool is_capture_pending()   const { return capture_pending_; }
    bool is_capturing()         const { return capture_mode_; }
    bool is_song_captured()     const { return capture_song_done_; }
    bool is_capture_session()   const { return capture_session_active_; }
    int  capture_song_rows_done()  const { return capture_song_rows_done_.load(); }
    int  capture_sfx_rows_done(int sfx_id) const {
        auto it = capture_sfx_rows_done_.find(sfx_id);
        return it != capture_sfx_rows_done_.end() ? it->second : 0;
    }
    int  capture_sfx_total_rows(int sfx_id) const {
        if(!sfx_defs_present_[sfx_id]) return 0;
        return sfx_defs_[sfx_id].song.num_rows;
    }
    // use_cache_: legacy public bool — assigning true sets both build and play,
    // assigning false clears both. Kept for backward compatibility.
    // Prefer set_build_cache() / set_play_cache() for finer control.
    bool use_cache_ = false;

    // Call after teardown() to prepare the player for a fresh init().
    // Does NOT close the SDL audio device — do that before calling reset().
    // keep_cache=true preserves song_cache_, sfx_cache_, and capture progress.
    // Use when restarting live playback without invalidating recorded cache.
    void reset(bool keep_cache = false) {
        ym_music_.reset(); ym_sfx_.reset();
        song_ = IngameFMSong{};
        defined_songs_.clear();
        if(!keep_cache) {
            song_cache_.clear();
            sfx_cache_.clear();
        }
        for(auto& d : sfx_defs_present_) d = false;
        for(auto& p : patches_present_)  p = false;
        for(auto& v : sfx_voice_)         v = SfxVoiceState{};
        for(auto& c : ch_state_)          c = IngameFMChannelState{};
        for(auto& p : pending_)           p = {};
        pending_song_ = PendingSong{};
        tick_rate_=60; speed_=6; samples_per_row_=0;
        current_row_.store(0); sample_in_row_=0;
        loop_=false; finished_.store(false);
        current_song_id_=-1;
        music_vol_.store(1.0f); sfx_vol_.store(1.0f);
        build_cache_=false; play_cache_=false; use_cache_=false;
        // Note: music_lfo_enable_, music_lfo_freq_, sfx_lfo_enable_, sfx_lfo_freq_
        // are intentionally NOT reset here — only set_music_lfo()/set_sfx_lfo() change them.
        if(!keep_cache) {
            capture_mode_=false; capture_pending_=false; capture_song_done_=false;
            capture_session_active_=false; sfx_voices_captured_=0;
            capture_song_rows_done_.store(0);
            capture_sfx_rows_done_.clear();
        } else {
            // Cancel any in-flight capture but keep the completed cache data
            capture_mode_=false; capture_pending_=false;
            capture_session_active_=false;
            if(sfx_voices_captured_ > 0) {
                sfx_voices_ = sfx_voices_captured_;
                sfx_voices_captured_ = 0;
            }
        }
        chip_type_=IngameFMChipType::YM3438;
        // sample_rate_ intentionally kept — caller sets it again before re-init
    }


    void song_dump(int id, const std::string& filename) {
        if(!build_cache_&&!use_cache_) { std::fprintf(stderr,"[IngameFM] song_dump: build_cache_/use_cache_ is false\n"); return; }
        auto it=song_cache_.find(id);
        if(it==song_cache_.end()||!it->second.valid) { std::fprintf(stderr,"[IngameFM] song_dump: no cache for ID %d\n",id); return; }
        write_cache_file(filename,it->second.samples,it->second.samples_per_row,it->second.total_rows,'M');
    }

    void sfx_dump(int id, const std::string& filename) {
        if(!build_cache_&&!use_cache_) { std::fprintf(stderr,"[IngameFM] sfx_dump: build_cache_/use_cache_ is false\n"); return; }
        auto it=sfx_cache_.find(id);
        if(it==sfx_cache_.end()||!it->second.valid) { std::fprintf(stderr,"[IngameFM] sfx_dump: no cache for ID %d\n",id); return; }
        write_cache_file(filename,it->second.samples,it->second.samples_per_row,it->second.total_rows,'S');
    }

    void song_from_xxd(int id, const uint8_t* data, size_t len, int tick_rate, int speed) {
        auto cached = parse_cache_data<CachedAudio>(data, len, 'M');
        if(!cached.valid) { std::fprintf(stderr,"[IngameFM] song_from_xxd: failed to parse ID %d\n",id); return; }
        // Use samples_per_row as stored in the file — it reflects the actual
        // sample rate at which the audio was rendered. Do NOT recalculate from
        // sample_rate_: the waveforms in the buffer are fixed at the recorded
        // rate and cannot be pitched/stretched by changing spr alone.
        // total_rows is also read from the file header; recalculating it would
        // give wrong results if the recorded rate differs from sample_rate_.
        song_cache_[id] = cached;
        DefinedSong def;
        def.tick_rate       = tick_rate;
        def.speed           = speed;
        def.samples_per_row = cached.samples_per_row; // from file
        def.cache_key_id    = id;
        def.valid           = true;
        def.song.num_rows   = cached.total_rows;      // from file
        def.song.num_channels = MAX_CHANNELS;
        def.song.rows.assign(cached.total_rows, make_empty_row(MAX_CHANNELS));
        defined_songs_[id]  = std::move(def);
        std::printf("[IngameFM] song_from_xxd: ID %d  rows=%d  spr=%d\n",
                    id, cached.total_rows, cached.samples_per_row);
    }

    void sfx_from_xxd(int id, const uint8_t* data, size_t len, int tick_rate, int speed) {
        auto cached = parse_cache_data<CachedAudio>(data, len, 'S');
        if(!cached.valid) { std::fprintf(stderr,"[IngameFM] sfx_from_xxd: failed to parse ID %d\n",id); return; }
        // Use spr and total_rows directly from the file header.
        sfx_cache_[id] = cached;
        SfxDef def;
        def.tick_rate       = tick_rate;
        def.speed           = speed;
        def.samples_per_row = cached.samples_per_row; // from file
        def.cache_key_id    = id;
        def.song.num_rows   = cached.total_rows;      // from file
        def.song.num_channels = 1;
        def.song.rows.assign(cached.total_rows, make_empty_row(1));
        sfx_defs_[id]       = std::move(def);
        sfx_defs_present_[id] = true;
        std::printf("[IngameFM] sfx_from_xxd: ID %d  rows=%d  spr=%d\n",
                    id, cached.total_rows, cached.samples_per_row);
    }

private:
    // ── Cache data ────────────────────────────────────────────────────────────
    // Unified struct for both song and SFX pre-rendered audio.
    // Samples are stereo interleaved int16: [L0,R0, L1,R1, ...]
    // Total samples = total_rows * samples_per_row * 2 (stereo)
    struct CachedAudio {
        std::vector<int16_t> samples;
        int samples_per_row = 0;
        int total_rows      = 0;
        bool valid          = false;
    };
    std::unordered_map<int, CachedAudio> song_cache_;
    std::unordered_map<int, CachedAudio> sfx_cache_;

    struct DefinedSong {
        IngameFMSong song;
        int tick_rate=60, speed=6, samples_per_row=0, cache_key_id=-1;
        bool valid=false;
    };
    std::unordered_map<int, DefinedSong> defined_songs_;
    int current_song_id_ = -1;

    struct SfxDef {
        IngameFMSong song;
        int tick_rate=60, speed=6, samples_per_row=0, cache_key_id=-1;
    };
    std::array<SfxDef,256>          sfx_defs_{};
    std::array<bool,256>            sfx_defs_present_{};
    int                             sfx_voices_=3;
    std::array<SfxVoiceState,MAX_SFX_VOICES> sfx_voice_{};

    IngameFMSong song_;
    int  tick_rate_=60, speed_=6, samples_per_row_=0;
    std::atomic<int>  current_row_{0};
    int  sample_in_row_=0;
    bool loop_=false;
    std::atomic<bool> finished_{false};
    std::array<IngameFMChannelState,MAX_CHANNELS> ch_state_{};
    struct PatchEntry {
        YM2612Patch patch;
        int  block      = 0;  // octave offset passed to set_frequency
        int  lfo_enable = 0;
        int  lfo_freq   = 0;
    };
    std::array<PatchEntry,256> patches_{};
    std::array<bool,256>       patches_present_{};

    struct PendingNote { bool has_note=false; bool is_off=false; int midi_note=0; int instId=0; int volume=0x7F; };
    std::array<PendingNote,MAX_CHANNELS> pending_{};

    struct PendingSong {
        IngameFMSong   song;
        int  tick_rate=60, speed=6, samples_per_row=0, start_row=0, song_id=-1;
        SongChangeWhen when=SongChangeWhen::AT_PATTERN_END;
        bool pending=false;
    };
    PendingSong pending_song_{};

    std::unique_ptr<IngameFMChip> ym_music_;
    std::unique_ptr<IngameFMChip> ym_sfx_;
    std::atomic<float> music_vol_{1.0f};
    std::atomic<float> sfx_vol_  {1.0f};

    int sample_rate_ = 44100;
    IngameFMChipType chip_type_ = IngameFMChipType::YM3438;
    bool music_lfo_enable_ = false; int music_lfo_freq_ = 0;
    bool sfx_lfo_enable_   = false; int sfx_lfo_freq_   = 0;

    // ── Live capture state ────────────────────────────────────────────────────
    bool capture_pending_        = false;  // waiting for next loop boundary
    bool capture_mode_           = false;  // actively capturing song
    bool capture_song_done_      = false;  // one full song loop captured
    bool capture_session_active_ = false;  // true from request until cancel/complete
    int  sfx_voices_captured_    = 0;      // saved sfx_voices_ value during session
    std::atomic<int>             capture_song_rows_done_{0};
    std::unordered_map<int,int>  capture_sfx_rows_done_;
    static constexpr int KEY_OFF_GAP_SAMPLES = 44;
    // Pre-allocated scratch buffer for SFX generate — avoids heap alloc per callback.
    // Sized for max samples_per_row at 48000 Hz with speed=12: 48000/60*12 = 9600 frames.
    static constexpr int SCRATCH_FRAMES = 9600;
    int16_t sfx_scratch_[SCRATCH_FRAMES * 2]{};

    // ── Helpers ───────────────────────────────────────────────────────────────
    static IngameFMRow make_empty_row(int num_channels) {
        IngameFMRow r;
        r.channels.resize(num_channels, {NOTE_NONE,-1,-1});
        return r;
    }

    void ensure_chips() {
        if(ym_music_) return;
        ym_music_=std::make_unique<IngameFMChip>();
        ym_sfx_  =std::make_unique<IngameFMChip>();
        ym_music_->set_chip_type(chip_type_);
        ym_sfx_->set_chip_type(chip_type_);
        ym_music_->enable_lfo(music_lfo_enable_, (uint8_t)music_lfo_freq_);
        ym_sfx_->enable_lfo(sfx_lfo_enable_,   (uint8_t)sfx_lfo_freq_);
        init_panning();
    }

    void init_panning() {
        for(int c=0;c<3;++c) {
            ym_music_->write(0,0xB4+c,0xC0); ym_music_->write(1,0xB4+c,0xC0);
            ym_sfx_  ->write(0,0xB4+c,0xC0); ym_sfx_  ->write(1,0xB4+c,0xC0);
        }
    }

    static YM2612Patch apply_volume(const YM2612Patch& src, int vol) {
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

    // ── Pre-render (cache build) ───────────────────────────────────────────────
    // Both song and SFX pre-render share the same logic: run a chip in isolation,
    // process each row, generate samples, store to a flat stereo int16 buffer.
    // The buffer layout is: [row0_sample0_L, row0_sample0_R, row0_sample1_L, ...]
    // i.e. sequential rows, sequential samples within each row, interleaved stereo.
    // This matches how audio_callback reads: base = (row * spr + sample_in_row) * 2

    static CachedAudio prerender(
        const IngameFMSong& song, int tick_rate, int speed, int num_channels,
        const std::array<PatchEntry,256>& patches,
        const std::array<bool,256>& patches_present,
        int sample_rate)
    {
        CachedAudio result;
        if(song.rows.empty()) return result;
        int spr = static_cast<int>(static_cast<double>(sample_rate)/tick_rate*speed);
        result.samples.resize(song.num_rows * spr * 2, 0);
        result.samples_per_row = spr;
        result.total_rows      = song.num_rows;
        result.valid           = true;

        IngameFMChip chip;
        for(int c=0;c<3;++c) { chip.write(0,0xB4+c,0xC0); chip.write(1,0xB4+c,0xC0); }

        // Per-channel state for multi-channel song pre-render
        std::array<IngameFMChannelState,MAX_CHANNELS> ch_state{};
        std::array<PendingNote,MAX_CHANNELS>           pending{};

        for(int rowIdx=0; rowIdx<song.num_rows; ++rowIdx) {
            if(rowIdx < (int)song.rows.size()) {
                const IngameFMRow& row = song.rows[rowIdx];
                int numCh = std::min((int)row.channels.size(), num_channels);
                for(int ch=0;ch<numCh;ch++) {
                    const IngameFMEvent& ev=row.channels[ch];
                    pending[ch]={};
                    if(ev.instrument>=0) ch_state[ch].instrument=ev.instrument;
                    if(ev.volume>=0)     ch_state[ch].volume=ev.volume;
                    if(ev.note==NOTE_OFF) {
                        chip.key_off(ch); ch_state[ch].active=false;
                    } else if(ev.note>=0) {
                        chip.key_off(ch); ch_state[ch].active=false;
                        pending[ch].has_note=true; pending[ch].midi_note=ev.note;
                        pending[ch].instId=ch_state[ch].instrument;
                        pending[ch].volume=ch_state[ch].volume;
                    }
                }
            }

            for(int s=0; s<spr; ++s) {
                if(s == KEY_OFF_GAP_SAMPLES) {
                    for(int ch=0;ch<num_channels;ch++) {
                        if(!pending[ch].has_note) continue;
                        int instId = pending[ch].instId;
                        int block  = 0;
                        if(patches_present[instId]) {
                            const PatchEntry& pe = patches[instId];
                            block = pe.block;
                            YM2612Patch p = apply_volume(pe.patch, pending[ch].volume);
                            chip.load_patch(p, ch);
                            chip.enable_lfo(pe.lfo_enable != 0,
                                            static_cast<uint8_t>(pe.lfo_freq));
                        }
                        chip.set_frequency(ch,
                            IngameFMChip::midi_to_hz(pending[ch].midi_note),
                            block, sample_rate);
                        chip.key_on(ch);
                        ch_state[ch].active=true; pending[ch].has_note=false;
                    }
                }
                int16_t frame[2];
                chip.generate(frame, 1, sample_rate);
                int idx = (rowIdx * spr + s) * 2;
                result.samples[idx]   = frame[0];
                result.samples[idx+1] = frame[1];
            }
        }
        return result;
    }

    static CachedAudio prerender_song(
        const IngameFMSong& song, int tick_rate, int speed,
        const std::array<PatchEntry,256>& patches,
        const std::array<bool,256>& patches_present, int sample_rate)
    {
        return prerender(song, tick_rate, speed, MAX_CHANNELS, patches, patches_present, sample_rate);
    }

    static CachedAudio prerender_sfx(
        const IngameFMSong& song, int tick_rate, int speed,
        const std::array<PatchEntry,256>& patches,
        const std::array<bool,256>& patches_present, int sample_rate)
    {
        return prerender(song, tick_rate, speed, 1, patches, patches_present, sample_rate);
    }

    // ── apply_pending_song ────────────────────────────────────────────────────
    void apply_pending_song() {
        PendingSong& ps=pending_song_;
        for(int ch=0;ch<MAX_CHANNELS;ch++) ym_music_->key_off(ch);
        for(auto& c:ch_state_) c=IngameFMChannelState{};
        for(auto& p:pending_)  p={};
        song_            = std::move(ps.song);
        tick_rate_       = ps.tick_rate;
        speed_           = ps.speed;
        samples_per_row_ = ps.samples_per_row;
        current_song_id_ = ps.song_id;
        current_row_.store(ps.start_row);
        sample_in_row_   = 0;
        finished_.store(false);
        ps.pending       = false;
        if(!(play_cache_||use_cache_)) {
            process_row(current_row_.load());
            commit_keyon();
            sample_in_row_ = KEY_OFF_GAP_SAMPLES;
        } else {
            sample_in_row_ = 0;
        }
    }

    // ── Music row processing ──────────────────────────────────────────────────
    void process_row(int rowIdx) {
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

    void commit_keyon() {
        for(int ch=0;ch<MAX_CHANNELS;ch++) {
            if(!pending_[ch].has_note) continue;
            int instId = pending_[ch].instId;
            int block  = 0;
            if(patches_present_[instId]) {
                const PatchEntry& pe = patches_[instId];
                block = pe.block;
                YM2612Patch p = apply_volume(pe.patch, pending_[ch].volume);
                ym_music_->load_patch(p, ch);
                ym_music_->enable_lfo(music_lfo_enable_, (uint8_t)music_lfo_freq_);
            }
            ym_music_->set_frequency(ch,
                IngameFMChip::midi_to_hz(pending_[ch].midi_note),
                block, sample_rate_);
            ym_music_->key_on(ch);
            ch_state_[ch].active=true; pending_[ch].has_note=false;
        }
    }

    // ── SFX voice pool ────────────────────────────────────────────────────────
    void sfx_process_row(int v, int rowIdx, bool skip_keyoff=false) {
        SfxVoiceState& vs=sfx_voice_[v];
        const SfxDef& def=sfx_defs_[vs.sfx_id];
        vs.pending_has_note=false; vs.pending_is_off=false;
        if(rowIdx>=(int)def.song.rows.size()) return;
        const IngameFMEvent& ev=def.song.rows[rowIdx].channels[0];
        if(ev.instrument>=0) vs.last_instrument=ev.instrument;
        if(ev.volume>=0)     vs.last_volume=ev.volume;
        if(ev.note==NOTE_OFF) {
            if(!skip_keyoff) ym_sfx_->key_off(v);
            vs.pending_is_off=true;
        } else if(ev.note>=0) {
            if(!skip_keyoff) ym_sfx_->key_off(v);
            vs.pending_has_note=true; vs.pending_note=ev.note;
            vs.pending_inst=vs.last_instrument; vs.pending_vol=vs.last_volume;
        }
    }

    void sfx_commit_keyon(int v) {
        SfxVoiceState& vs=sfx_voice_[v];
        if(!vs.pending_has_note) return;
        int block = 0;
        if(patches_present_[vs.pending_inst]) {
            const PatchEntry& pe = patches_[vs.pending_inst];
            block = pe.block;
            YM2612Patch p = apply_volume(pe.patch, vs.pending_vol);
            ym_sfx_->load_patch(p, v);
            ym_sfx_->enable_lfo(sfx_lfo_enable_, (uint8_t)sfx_lfo_freq_);
        }
        ym_sfx_->set_frequency(v,
            IngameFMChip::midi_to_hz(vs.pending_note),
            block, sample_rate_);
        ym_sfx_->key_on(v);
        vs.pending_has_note=false;
    }

    void sfx_tick_voice(int v, int samples) {
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
                // Track SFX capture progress
                if(capture_session_active_) {
                    auto it=capture_sfx_rows_done_.find(vs.sfx_id);
                    if(it!=capture_sfx_rows_done_.end()) {
                        int prev = it->second;
                        it->second = std::min(it->second+1, def.song.num_rows);
                        if(it->second != prev)
                            std::printf("[IngameFM] SFX %d rows_done=%d/%d\n",
                                        vs.sfx_id, it->second, def.song.num_rows);
                    }
                }
                if(vs.ticks_remaining<=0) { ym_sfx_->key_off(v); vs=SfxVoiceState{}; return; }
                vs.current_row++;
                // During tail (past pattern end): don't process new rows, just let chip ring down
                if(vs.current_row < (int)def.song.rows.size())
                    sfx_process_row(v, vs.current_row);
            }
        }
    }

    // ── Audio callback ────────────────────────────────────────────────────────
    void audio_callback(int16_t* stream, int samples) {
        if(finished_.load()) { std::memset(stream,0,samples*4); return; }

        // Immediate song change
        if(pending_song_.pending && pending_song_.when==SongChangeWhen::NOW)
            apply_pending_song();

        int remaining=samples;
        int16_t* out=stream;

        while(remaining>0) {
            int  pos_in_row   = sample_in_row_;
            // In cache mode there is no key_off gap — the gap is pre-baked.
            // Always use samples_per_row_ as the boundary.
            bool in_gap       = !(play_cache_||use_cache_) && (pos_in_row < KEY_OFF_GAP_SAMPLES);
            // In cached playback, use the cache's own spr so tempo matches exactly
            // what was recorded regardless of any re-definition at a different rate.
            int  cache_spr    = samples_per_row_;
            if((play_cache_||use_cache_) && current_song_id_>=0) {
                auto cit = song_cache_.find(current_song_id_);
                if(cit!=song_cache_.end() && cit->second.valid)
                    cache_spr = cit->second.samples_per_row;
            }
            int  next_boundary= in_gap ? KEY_OFF_GAP_SAMPLES : cache_spr;
            int  to_generate  = std::min(remaining, next_boundary-pos_in_row);
            const float mv    = music_vol_.load();
            const float sv    = sfx_vol_.load();
            const int   n     = to_generate*2;

            // ── Music output ─────────────────────────────────────────────────
            // Prefer cache if available; fall back to live chip.
            bool used_cache_music = false;
            if((play_cache_||use_cache_) && current_song_id_>=0) {
                auto it = song_cache_.find(current_song_id_);
                if(it!=song_cache_.end() && it->second.valid) {
                    const CachedAudio& c = it->second;
                    // Index into the flat buffer: each row is spr stereo frames.
                    // sample_in_row_ counts mono samples, so multiply by 2 for stereo.
                    int base = (current_row_.load() * c.samples_per_row + pos_in_row) * 2;
                    if(base+n <= (int)c.samples.size()) {
                        std::memcpy(out, &c.samples[base], n*sizeof(int16_t));
                        used_cache_music = true;
                    }
                }
            }
            if(!used_cache_music) {
                if(ym_music_) ym_music_->generate(out, to_generate, sample_rate_);
                else std::memset(out, 0, n*sizeof(int16_t));
            }
            if(mv<1.0f)
                for(int i=0;i<n;++i) out[i]=static_cast<int16_t>(out[i]*mv);

            // ── Live capture: write music output into song cache ──────────────
            if(capture_mode_ && !used_cache_music && current_song_id_>=0) {
                auto it = song_cache_.find(current_song_id_);
                if(it != song_cache_.end()) {
                    int base = (current_row_.load() * it->second.samples_per_row + pos_in_row) * 2;
                    if(base+n <= (int)it->second.samples.size()) {
                        std::memcpy(&it->second.samples[base], out, n*sizeof(int16_t));
                    }
                }
            }

            // ── SFX output ───────────────────────────────────────────────────
            bool any_sfx=false;
            for(int v=0;v<sfx_voices_;++v) if(sfx_voice_[v].active()){any_sfx=true;break;}

            if(any_sfx && sv>0.0f) {
                if(play_cache_||use_cache_) {
                    // Mix each active SFX voice from its pre-rendered buffer.
                    for(int frame=0; frame<to_generate; ++frame) {
                        float mixL = static_cast<float>(out[frame*2]);
                        float mixR = static_cast<float>(out[frame*2+1]);
                        for(int v=0;v<sfx_voices_;++v) {
                            if(!sfx_voice_[v].active()) continue;
                            const SfxVoiceState& vs = sfx_voice_[v];
                            auto it = sfx_cache_.find(vs.cache_key_id);
                            if(it==sfx_cache_.end()||!it->second.valid) continue;
                            const CachedAudio& c = it->second;
                            int sfx_frame = vs.current_row * c.samples_per_row
                                          + vs.sample_in_row + frame;
                            int sfx_idx   = sfx_frame * 2;
                            if(sfx_idx+1 < (int)c.samples.size()) {
                                mixL += static_cast<float>(c.samples[sfx_idx])   * sv;
                                mixR += static_cast<float>(c.samples[sfx_idx+1]) * sv;
                            }
                        }
                        out[frame*2]   = static_cast<int16_t>(std::max(-32768.f,std::min(32767.f,mixL)));
                        out[frame*2+1] = static_cast<int16_t>(std::max(-32768.f,std::min(32767.f,mixR)));
                    }
                } else {
                    if(ym_sfx_) {
                        ym_sfx_->generate(sfx_scratch_, to_generate, sample_rate_);
                        for(int i=0;i<n;++i) {
                            float m=static_cast<float>(out[i])+static_cast<float>(sfx_scratch_[i])*sv;
                            out[i]=static_cast<int16_t>(std::max(-32768.f,std::min(32767.f,m)));
                        }
                        // ── Capture SFX output ───────────────────────────────
                        if(capture_session_active_) {
                            for(int v=0;v<sfx_voices_;++v) {
                                if(!sfx_voice_[v].active()) continue;
                                const SfxVoiceState& vs = sfx_voice_[v];
                                int sid = vs.sfx_id;
                                auto it = sfx_cache_.find(sid);
                                if(it==sfx_cache_.end()||!it->second.valid) continue;
                                for(int frame=0;frame<to_generate;++frame) {
                                    int sfx_frame = vs.current_row * it->second.samples_per_row
                                                  + vs.sample_in_row + frame;
                                    int idx = sfx_frame*2;
                                    if(idx+1 < (int)it->second.samples.size()) {
                                        it->second.samples[idx]   = sfx_scratch_[frame*2];
                                        it->second.samples[idx+1] = sfx_scratch_[frame*2+1];
                                    }
                                }
                            }
                        }
                    }
                }
            } else {
                // No SFX active — still advance ym_sfx_ clock to keep it in sync
                if(!(play_cache_||use_cache_) && ym_sfx_) {
                    ym_sfx_->generate(sfx_scratch_, to_generate, sample_rate_);
                }
            }

            // ── Tick SFX voice cursors ────────────────────────────────────────
            for(int v=0;v<sfx_voices_;++v) sfx_tick_voice(v,to_generate);

            out            += to_generate*2;
            remaining      -= to_generate;
            sample_in_row_ += to_generate;

            // When using cache, skip process_row / commit_keyon — they only
            // drive ym_music_ which is not used in cache mode. The baked audio
            // already contains the correct envelopes and notes.
            if(!(play_cache_||use_cache_)) {
                if(in_gap && sample_in_row_>=KEY_OFF_GAP_SAMPLES) commit_keyon();
            }

            if(sample_in_row_>=cache_spr) {
                sample_in_row_=0;
                int row=current_row_.load()+1;

                if(row>=(int)song_.rows.size()) {
                    // ── Loop boundary ─────────────────────────────────────────
                    // If capture was pending, activate it now (start of new loop)
                    if(capture_pending_ && !capture_song_done_) {
                        capture_pending_ = false;
                        capture_mode_    = true;
                        capture_song_rows_done_.store(0);
                        // Allocate fresh song cache buffer
                        if(current_song_id_ >= 0 && defined_songs_.count(current_song_id_)) {
                            const DefinedSong& def = defined_songs_[current_song_id_];
                            CachedAudio c;
                            c.samples_per_row = def.samples_per_row;
                            c.total_rows      = def.song.num_rows;
                            c.samples.assign(def.song.num_rows * def.samples_per_row * 2, 0);
                            c.valid = false;
                            song_cache_[current_song_id_] = std::move(c);
                        }
                    }
                    // If currently capturing, one loop just finished — mark song done
                    else if(capture_mode_) {
                        if(current_song_id_ >= 0) {
                            auto it = song_cache_.find(current_song_id_);
                            if(it != song_cache_.end()) it->second.valid = true;
                        }
                        capture_mode_      = false;
                        capture_song_done_ = true;
                        capture_song_rows_done_.store((int)song_.rows.size());
                    }

                    if(pending_song_.pending &&
                       pending_song_.when==SongChangeWhen::AT_PATTERN_END) {
                        apply_pending_song();
                        break;
                    }
                    if(loop_) { for(auto& p:pending_) p={}; row=0; }
                    else {
                        std::memset(out,0,remaining*4);
                        if(ym_music_) for(int ch=0;ch<MAX_CHANNELS;ch++) ym_music_->key_off(ch);
                        finished_.store(true); return;
                    }
                } else if(capture_mode_) {
                    // Mid-loop: update rows-done counter
                    capture_song_rows_done_.store(row);
                }

                current_row_.store(row);
                if(!(play_cache_||use_cache_)) process_row(row);
            }
        }
    }

    // ── Cache file I/O ────────────────────────────────────────────────────────
    // File format: 4-byte magic "IF?C" + 3x uint32 (spr, rows, sample_count) + int16 data
    static void write_cache_file(const std::string& fn,
                                  const std::vector<int16_t>& samples,
                                  int spr, int rows, char type) {
        FILE* f=std::fopen(fn.c_str(),"wb");
        if(!f) { std::perror(("open "+fn).c_str()); return; }
        char magic[4]={'I','F',type,'C'};
        uint32_t u_spr=(uint32_t)spr, u_rows=(uint32_t)rows, u_count=(uint32_t)samples.size();
        std::fwrite(magic,1,4,f);
        std::fwrite(&u_spr,4,1,f); std::fwrite(&u_rows,4,1,f); std::fwrite(&u_count,4,1,f);
        if(!samples.empty()) std::fwrite(samples.data(),2,samples.size(),f);
        std::fclose(f);
        std::printf("[IngameFM] wrote %s  (%u samples, type=%c)\n",fn.c_str(),u_count,type);
    }

    template<typename T>
    static T parse_cache_data(const uint8_t* data, size_t len, char expectedType) {
        T result; result.valid=false;
        if(len<16) { std::fprintf(stderr,"[IngameFM] cache data too short\n"); return result; }
        if(data[0]!='I'||data[1]!='F'||data[2]!=(uint8_t)expectedType||data[3]!='C') {
            std::fprintf(stderr,"[IngameFM] bad magic (expected IF%cC)\n",expectedType);
            return result;
        }
        uint32_t spr,rows,count;
        std::memcpy(&spr,  data+4,  4);
        std::memcpy(&rows, data+8,  4);
        std::memcpy(&count,data+12, 4);
        if(len < 16+count*2) { std::fprintf(stderr,"[IngameFM] cache data truncated\n"); return result; }
        result.samples_per_row = (int)spr;
        result.total_rows      = (int)rows;
        result.samples.resize(count);
        std::memcpy(result.samples.data(), data+16, count*2);
        result.valid=true;
        return result;
    }
};
