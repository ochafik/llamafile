// vlib_video_session.cpp — continuous-session state machine (mtmd-retargeted).
//
// Per-frame flow:
//   1. frame_count++
//   2. if (audio_text): decode "\n[Audio transcript]: ..." text (persists across rewinds)
//   3. rewind_floor = n_past
//   4. ref = ref_set ? ref_frame : cur_frame  ("last frame I reacted to")
//   5. build (ref, cur) bitmaps + per-frame marker text; mtmd_tokenize auto-merges
//      the same-size pair into one temporal image chunk (QWEN_VIDEO n_batch==2).
//   6. eval each chunk via mtmd_helper_eval_chunk_single (encode+KV-decode+M-RoPE);
//      push one cumulative grid_thw entry for the merged image chunk.
//   7. generate up to max_tool_tokens; stop on </tool_call>, EOS, or budget.
//   8. parse generated text -> tool_call.
//   9. if action == do_nothing/ignore_frame (sole):
//          llama_memory_seq_rm_attn(rewind_floor, -1); n_past = rewind_floor;
//          pop the cumulative grid_thw entry we pushed for this frame.
//      else (speak / note / other): ref = cur; n_past stays advanced.
//
// M-RoPE rewind invariant (test_mrope U4b): each kept frame contributes one
// {t=1,h,w} entry to a running cumulative stack; ignore_frame must pop the EXACT
// entry it pushed or every subsequent frame desyncs. llama.cpp computes RoPE
// per-token from explicit batch positions, so trimming KV via
// llama_memory_seq_rm_attn + restoring n_past is sufficient (no internal
// rope-delta accumulator to resync). Recurrent state is intentionally NOT rolled
// back — seq_rm_attn drops only the attention KV (MLX ghost-memory semantic).

#include "vlib_video_session.h"

#include "mtmd.h"
#include "mtmd-helper.h"
#include "llama.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace vlib {

namespace {

std::string fmt(const char * f, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, f);
    vsnprintf(buf, sizeof(buf), f, ap);
    va_end(ap);
    return std::string(buf);
}

class session_impl : public session {
public:
    session_impl(llama_context * lctx,
                 const llama_model * model,
                 mtmd_context  * ctx_mtmd,
                 const session_params & params)
        : m_lctx(lctx),
          m_model(model),
          m_mtmd(ctx_mtmd),
          m_params(params),
          m_n_past(0),
          m_sys_pos_end(0),
          m_frame_count(0),
          m_seq_id(params.seq_id),
          m_ref_set(false) {}

    int32_t start() override {
        if (m_params.system_prompt.empty()) {
            m_sys_pos_end = 0;
            return 0;
        }
        std::vector<llama_token> toks =
            tokenize(m_params.system_prompt, /*add_special=*/true, /*parse_special=*/true);
        if (toks.empty()) return -1;
        if (decode_text_tokens(toks) != 0) return -2;
        m_sys_pos_end = m_n_past;
        return 0;
    }

    action process_frame(const unsigned char * cur_rgb, const std::string & audio_text) override {
        action result;
        m_frame_count++;

        if (m_mtmd == nullptr || m_params.nx == 0 || m_params.ny == 0 || cur_rgb == nullptr) {
            return result;
        }

        // Step 2: optional audio transcript chunk (persists across rewinds).
        if (!audio_text.empty()) {
            std::string s = fmt("\n[Audio transcript]: %s\n", audio_text.c_str());
            std::vector<llama_token> toks = tokenize(s, /*add_special=*/false, /*parse_special=*/false);
            if (!toks.empty() && decode_text_tokens(toks) != 0) {
                return result;
            }
        }

        // Step 3: snapshot rewind floor (above sys_pos_end and any audio).
        const llama_pos rewind_floor = m_n_past;

        // Step 4: reference frame selection.
        const unsigned char * ref = m_ref_set ? m_ref_frame.data() : cur_rgb;

        // Step 5: build the (ref, cur) bitmap pair + per-frame marker text.
        // Same nx/ny -> mtmd_tokenize temporally merges them into one frame.
        mtmd::bitmap bmp_ref(m_params.nx, m_params.ny, ref);
        mtmd::bitmap bmp_cur(m_params.nx, m_params.ny, cur_rgb);
        const mtmd_bitmap * bmps[2] = { bmp_ref.ptr.get(), bmp_cur.ptr.get() };

        // IMPORTANT: mtmd_tokenize requires one media marker per bitmap (the
        // count is checked BEFORE merging). We feed the (ref, cur) pair as TWO
        // ADJACENT markers; mtmd's can_merge_with() then temporally merges the
        // two same-size bitmaps into ONE image chunk (QWEN_VIDEO n_batch==2),
        // collapsing the second marker. Non-adjacent markers would NOT merge.
        const std::string marker =
            m_params.media_marker.empty() ? std::string(mtmd_default_marker())
                                          : m_params.media_marker;
        std::string label = fmt(m_params.per_frame_label_format.c_str(), (int) m_frame_count);
        std::string text  = m_params.per_frame_user_prefix + label + marker + marker +
                            m_params.per_frame_instruction + m_params.per_frame_user_suffix;

        mtmd::input_chunks chunks(mtmd_input_chunks_init());
        mtmd_input_text itext{ text.c_str(), /*add_special=*/false, /*parse_special=*/true };
        int32_t trc = mtmd_tokenize(m_mtmd, chunks.ptr.get(), &itext, bmps, 2);
        if (trc != 0) {
            // trc==1: bitmap/marker count mismatch (pair did NOT merge — sizes
            // differ); trc==2: preprocessing error. Either way, hold the frame.
            return result;
        }

        // Step 6: eval each chunk (text -> tokens; image -> encode+KV+M-RoPE),
        // tracking n_past explicitly for M-RoPE (n_pos != n_tokens).
        llama_pos n_past = m_n_past;
        bool pushed_grid = false;
        const size_t n_chunks = mtmd_input_chunks_size(chunks.ptr.get());
        for (size_t i = 0; i < n_chunks; ++i) {
            const mtmd_input_chunk * ch = mtmd_input_chunks_get(chunks.ptr.get(), i);
            const bool is_text = mtmd_input_chunk_get_type(ch) == MTMD_INPUT_CHUNK_TYPE_TEXT;

            llama_pos new_n_past = n_past;
            int32_t rc = mtmd_helper_eval_chunk_single(
                m_mtmd, m_lctx, ch,
                n_past, m_seq_id, m_params.n_batch,
                /*logits_last=*/true, &new_n_past);
            if (rc != 0) {
                // Best-effort: the KV may have partially advanced. Reflect that
                // and bail; the caller can choose to rewind_last().
                m_n_past = new_n_past;
                m_pending_rewind_floor = rewind_floor;
                m_pending_pop_grid     = pushed_grid;
                return result;
            }
            if (!is_text && !pushed_grid) {
                // One cumulative {t=1,h,w} entry for the merged (ref,cur) frame.
                const uint32_t patch = 14, merge = 2;
                grid_entry ge;
                ge.t = 1;
                ge.h = m_params.ny / patch / merge;
                ge.w = m_params.nx / patch / merge;
                m_grid_thw.push_back(ge);
                pushed_grid = true;
            }
            n_past = new_n_past;
        }
        m_n_past = n_past;
        m_pending_rewind_floor = rewind_floor;
        m_pending_pop_grid     = pushed_grid;

        // Step 7: generate until </tool_call>, EOS, or budget.
        std::string assistant_text = generate_until_stop(m_params.max_tool_tokens);
        result.raw_assistant_text = assistant_text;

        // Step 8: parse.
        tool_call tc;
        bool parsed = parse_tool_call(assistant_text, tc);
        if (!parsed) {
            // No parseable tool call — keep KV, hold current frame as ref.
            result.kind = ACTION_NONE;
            consume_frame(cur_rgb);
            m_pending_rewind_floor = -1;
            m_pending_pop_grid     = false;
            return result;
        }
        result.call = tc;

        // Step 9: dispatch.
        const std::string & name = tc.name;
        const bool is_ignore = (name == "do_nothing" || name == "ignore_frame");
        if (is_ignore) {
            result.kind = ACTION_DO_NOTHING;
            // Trim the ATTENTION KV back to the pre-frame mark so the ignored
            // frame leaves no attention trace. We use llama_memory_seq_rm_attn
            // (an attention-only variant of seq_rm; see the llama.cpp submodule
            // commit on branch llamafile-2026-ghostkv). On hybrid
            // (attention+recurrent) models — e.g. Qwen3.5/3.6 gated-delta-net —
            // a plain llama_memory_seq_rm tries to roll back the recurrent
            // sub-cache first and, because a frame-sized rollback exceeds the
            // per-token recurrent snapshot depth (n_rs_seq), REFUSES and leaves
            // BOTH caches unmutated (returns false) — the whole frame is kept.
            // seq_rm_attn instead drops only the attention KV and KEEPS the
            // recurrent state, so the boring frame leaves just a faint trace in
            // the recurrent memory (the intended "ghost memory" / MLX watchdawg
            // semantic) while attention positions stay contiguous. On
            // pure-attention models (e.g. Qwen2.5-VL) it is identical to
            // llama_memory_seq_rm, so behaviour there is unchanged.
            llama_memory_t mem = llama_get_memory(m_lctx);
            bool removed = mem ? llama_memory_seq_rm_attn(mem, m_seq_id, rewind_floor, -1) : false;
            if (removed) {
                m_n_past = rewind_floor;
                if (m_pending_pop_grid && !m_grid_thw.empty()) {
                    m_grid_thw.pop_back();
                }
            }
            // else: nothing mutated; keep m_n_past + grid at the post-frame
            // values so the sequence stays position-consistent.
            m_pending_pop_grid     = false;
            m_pending_rewind_floor = -1;
            return result;
        }

        // Speak / note / other: cur becomes the new ref; n_past stays advanced.
        consume_frame(cur_rgb);
        m_pending_rewind_floor = -1;
        m_pending_pop_grid     = false;
        if (name == "speak") {
            result.kind = ACTION_SPEAK;
            if (m_speak_cb) {
                auto it = tc.arguments.find("text");
                m_speak_cb(it != tc.arguments.end() ? it->second : std::string{}, m_speak_user);
            }
        } else if (name == "note") {
            result.kind = ACTION_NOTE;
            if (m_note_cb) {
                auto it = tc.arguments.find("observation");
                m_note_cb(it != tc.arguments.end() ? it->second : std::string{}, m_note_user);
            }
        } else {
            result.kind = ACTION_OTHER;
        }
        return result;
    }

    action continue_after_tool(const std::string & tool_result) override {
        action result;
        if (m_frame_count == 0) return result;  // no frame yet

        // Decode the tool-response turn (kept in KV; no rewind — the frame
        // already produced a real reaction by calling the external tool).
        std::string text = m_params.tool_response_prefix + tool_result +
                           m_params.tool_response_suffix;
        std::vector<llama_token> toks = tokenize(text, /*add_special=*/false, /*parse_special=*/true);
        if (!toks.empty() && decode_text_tokens(toks) != 0) {
            result.kind = ACTION_NONE;
            return result;
        }

        // Resume generation and parse the next action.
        std::string assistant_text = generate_until_stop(m_params.max_tool_tokens);
        result.raw_assistant_text = assistant_text;

        tool_call tc;
        if (!parse_tool_call(assistant_text, tc)) {
            result.kind = ACTION_NONE;
            return result;
        }
        result.call = tc;

        const std::string & name = tc.name;
        if (name == "do_nothing" || name == "ignore_frame") {
            // In a continuation we do NOT rewind: the frame already reacted.
            result.kind = ACTION_DO_NOTHING;
        } else if (name == "speak") {
            result.kind = ACTION_SPEAK;
            if (m_speak_cb) {
                auto it = tc.arguments.find("text");
                m_speak_cb(it != tc.arguments.end() ? it->second : std::string{}, m_speak_user);
            }
        } else if (name == "note") {
            result.kind = ACTION_NOTE;
            if (m_note_cb) {
                auto it = tc.arguments.find("observation");
                m_note_cb(it != tc.arguments.end() ? it->second : std::string{}, m_note_user);
            }
        } else {
            result.kind = ACTION_OTHER;
        }
        return result;
    }

    void rewind_last() override {
        if (m_pending_rewind_floor < 0) return;
        llama_memory_t mem = llama_get_memory(m_lctx);
        bool removed = mem ? llama_memory_seq_rm_attn(mem, m_seq_id, m_pending_rewind_floor, -1) : false;
        if (removed) {
            m_n_past = m_pending_rewind_floor;
            if (m_pending_pop_grid && !m_grid_thw.empty()) {
                m_grid_thw.pop_back();
            }
        }
        // else: attention trim failed (no memory / unexpected); nothing mutated,
        // so leave m_n_past/grid at their post-frame values (see process_frame).
        // seq_rm_attn keeps the recurrent state intact on hybrid models, so it no
        // longer refuses a frame-sized attention rollback the way seq_rm did.
        m_pending_pop_grid     = false;
        m_pending_rewind_floor = -1;
    }

    void set_speak_cb(speak_cb cb, void * cb_user) override { m_speak_cb = std::move(cb); m_speak_user = cb_user; }
    void set_note_cb (note_cb  cb, void * cb_user) override { m_note_cb  = std::move(cb); m_note_user  = cb_user; }

    llama_pos n_past() const override { return m_n_past; }
    int32_t   frame_count() const override { return m_frame_count; }
    size_t    cumulative_grid_size() const override { return m_grid_thw.size(); }

    uint32_t cumulative_t_sum() const override {
        uint32_t s = 0;
        for (const auto & e : m_grid_thw) s += e.t;
        return s;
    }

private:
    struct grid_entry { uint32_t t; uint32_t h; uint32_t w; };

    void consume_frame(const unsigned char * cur_rgb) {
        const size_t need = (size_t) m_params.nx * (size_t) m_params.ny * 3;
        m_ref_frame.assign(cur_rgb, cur_rgb + need);
        m_ref_set = true;
    }

    std::vector<llama_token> tokenize(const std::string & text, bool add_special, bool parse_special) {
        if (m_model == nullptr) return {};
        const llama_vocab * vocab = llama_model_get_vocab(m_model);
        if (!vocab) return {};
        int32_t need = -llama_tokenize(vocab, text.c_str(), (int32_t) text.size(),
                                       nullptr, 0, add_special, parse_special);
        if (need <= 0) return {};
        std::vector<llama_token> out(need);
        int32_t got = llama_tokenize(vocab, text.c_str(), (int32_t) text.size(),
                                     out.data(), (int32_t) out.size(),
                                     add_special, parse_special);
        if (got < 0) return {};
        out.resize(got);
        return out;
    }

    int decode_text_tokens(const std::vector<llama_token> & toks) {
        if (toks.empty()) return 0;
        const int32_t n_batch = (int32_t) std::min<uint32_t>(
            (uint32_t) std::max(1, m_params.n_batch), (uint32_t) toks.size());
        for (size_t off = 0; off < toks.size(); off += (size_t) n_batch) {
            int32_t this_batch = (int32_t) std::min<size_t>((size_t) n_batch, toks.size() - off);
            llama_batch b = llama_batch_init(this_batch, 0, 1);
            for (int32_t i = 0; i < this_batch; ++i) {
                b.token   [b.n_tokens] = toks[off + i];
                b.pos     [b.n_tokens] = m_n_past + (int32_t) off + i;
                b.n_seq_id[b.n_tokens] = 1;
                b.seq_id  [b.n_tokens][0] = m_seq_id;
                bool emit = (off + (size_t) i + 1 == toks.size());
                b.logits  [b.n_tokens] = emit ? 1 : 0;
                b.n_tokens++;
            }
            int rc = llama_decode(m_lctx, b);
            llama_batch_free(b);
            if (rc != 0) return rc;
        }
        m_n_past += (llama_pos) toks.size();
        return 0;
    }

    std::string generate_until_stop(int32_t max_tokens) {
        if (m_model == nullptr) return {};
        const llama_vocab * vocab = llama_model_get_vocab(m_model);
        if (!vocab) return {};
        std::string out;
        const llama_token tok_eos = llama_vocab_eos(vocab);

        for (int32_t i = 0; i < max_tokens; ++i) {
            float * logits = llama_get_logits_ith(m_lctx, -1);
            if (!logits) break;
            const int32_t n_vocab = llama_vocab_n_tokens(vocab);

            llama_token best = 0;
            float best_v = logits[0];
            for (int32_t t = 1; t < n_vocab; ++t) {
                if (logits[t] > best_v) { best_v = logits[t]; best = (llama_token) t; }
            }
            if (best == tok_eos) break;

            char buf[64];
            int32_t n = llama_token_to_piece(vocab, best, buf, sizeof(buf), /*lstrip=*/0, /*special=*/true);
            if (n > 0) out.append(buf, buf + n);

            llama_batch b = llama_batch_init(1, 0, 1);
            b.token   [0] = best;
            b.pos     [0] = m_n_past;
            b.n_seq_id[0] = 1;
            b.seq_id  [0][0] = m_seq_id;
            b.logits  [0] = 1;
            b.n_tokens = 1;
            int rc = llama_decode(m_lctx, b);
            llama_batch_free(b);
            if (rc != 0) break;
            m_n_past++;

            if (out.find("</tool_call>") != std::string::npos) break;
        }
        return out;
    }

    llama_context     * m_lctx;
    const llama_model * m_model;
    mtmd_context      * m_mtmd;
    session_params      m_params;

    llama_pos     m_n_past;
    llama_pos     m_sys_pos_end;
    int32_t       m_frame_count;
    llama_seq_id  m_seq_id;

    bool                       m_ref_set;
    std::vector<unsigned char> m_ref_frame;

    std::vector<grid_entry>    m_grid_thw;
    llama_pos                  m_pending_rewind_floor = -1;
    bool                       m_pending_pop_grid     = false;

    speak_cb m_speak_cb;
    void *   m_speak_user = nullptr;
    note_cb  m_note_cb;
    void *   m_note_user  = nullptr;
};

} // namespace

std::unique_ptr<session> session::create(llama_context * lctx,
                                         const llama_model * model,
                                         mtmd_context  * ctx_mtmd,
                                         const session_params & params) {
    return std::unique_ptr<session>(new session_impl(lctx, model, ctx_mtmd, params));
}

} // namespace vlib
