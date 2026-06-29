// vlib_video_session.h — continuous video session state machine.
//
// Mirrors mlx_vlm/continuous_analyzer.py:ContinuousSession closely enough that
// recordings from one can be replayed against the other:
//
//   - Per-frame: build a (ref, cur) image pair, tokenize it (mtmd auto-merges
//     the two same-size bitmaps into one temporal frame via the QWEN_VIDEO
//     n_batch==2 path), decode, generate until </tool_call> or EOS, parse,
//     dispatch the action.
//   - On do_nothing/ignore_frame (sole action): rewind the attention KV cache
//     to the pre-frame mark and pop this frame's M-RoPE grid entry. For hybrid
//     models the recurrent state is intentionally NOT rolled back (matches the
//     MLX "ghost memory" semantic — see CONTINUOUS.md).
//   - On speak/note/other: cur becomes the new ref; n_past stays advanced.
//
// PORT NOTE (vs the video-conv3d prior art): the custom vlib_video_encoder +
// MTMD_INPUT_CHUNK_TYPE_VIDEO path is DROPPED. The per-frame encode is retargeted
// onto upstream mtmd's public API:
//   mtmd_tokenize(ctx, chunks, &text, bitmaps[2], 2)         // auto-merges pair
//   mtmd_helper_eval_chunk_single(...)                       // encode+KV+M-RoPE
//   llama_memory_seq_rm(...)                                 // rewind (pure llama.h)
// No ffmpeg, no custom graph, no llama.cpp-internal headers.
//
// The session does not own any inference handles — the caller hands in the
// llama_context, llama_model, and mtmd_context, and is responsible for freeing
// them after the session is destroyed.

#pragma once

#include "vlib_video_tool_parser.h"

// NB: include llama-cpp.h (which lives in llama.cpp/include/) rather than
// "llama.h" directly. From a file under llamafile/, `#include "llama.h"`
// resolves to the llamafile-owned wrapper (llamafile/llama.h) which only
// forward-declares the structs; llama-cpp.h pulls in the REAL llama.h (its own
// quoted include resolves relative to llama.cpp/include/), giving us the
// llama_pos / llama_seq_id / llama_token typedefs this header needs.
#include "llama-cpp.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

struct mtmd_context;

namespace vlib {

// Action returned by process_frame.
enum action_kind {
    ACTION_NONE       = 0, // parser failed; session held current frame
    ACTION_DO_NOTHING = 1, // canonical "no signal" — caused a rewind
    ACTION_SPEAK      = 2,
    ACTION_NOTE       = 3,
    ACTION_OTHER      = 4, // a tool name we don't recognize (still kept in KV)
};

struct action {
    action_kind kind = ACTION_NONE;
    tool_call   call;
    std::string raw_assistant_text; // verbatim model output for this frame
};

// Callbacks fired when the session decides to speak or note. cb_user is the
// opaque user pointer passed via set_*_cb.
using speak_cb = std::function<void(const std::string & text, void * cb_user)>;
using note_cb  = std::function<void(const std::string & observation, void * cb_user)>;

struct session_params {
    // Sequence id to use for KV cache writes. Default 0.
    llama_seq_id seq_id = 0;

    // Preprocessed frame size in pixels. Both ref and cur are fed to mtmd at
    // this exact size so mtmd's can_merge_with() path temporally merges them.
    // The caller is responsible for resizing decoded JPEGs to (nx, ny).
    uint32_t nx = 0;
    uint32_t ny = 0;

    // llama_decode batch size used when evaluating chunks. Default 512.
    int32_t n_batch = 512;

    // Generation cap per frame. Typical tool-call output is < 64 tokens; this
    // is the hard ceiling.
    int32_t max_tool_tokens = 256;

    // System prompt prepended once at start(). Tokens emitted from this prompt
    // are pinned — rewinds never go below sys_pos_end.
    std::string system_prompt;

    // Per-frame chat-template fragments. mtmd inserts the model-specific vision
    // boundary tokens itself when it expands the media marker, so we only wrap
    // the marker with the chat turn scaffolding.
    std::string per_frame_user_prefix  = "<|im_start|>user\n";
    std::string per_frame_user_suffix  = "<|im_end|>\n<|im_start|>assistant\n";
    std::string media_marker;            // empty -> mtmd_default_marker()
    std::string per_frame_label_format = "[Frame %d] "; // %d -> frame index
    std::string per_frame_instruction  = "\nAnalyze and call a tool.";

    // Chat-template fragments for feeding an EXTERNAL tool's result back into
    // the session (the ACTION_OTHER mini tool-loop, doc 03 §agentic-loop). The
    // tool result text is wrapped between these and decoded as a fresh turn,
    // then generation resumes WITHOUT a new frame / rewind. Defaults follow
    // Qwen's <tool_response> convention.
    std::string tool_response_prefix = "<|im_start|>user\n<tool_response>\n";
    std::string tool_response_suffix = "\n</tool_response><|im_end|>\n<|im_start|>assistant\n";
};

struct session {
    // Construct a session bound to live llama/mtmd handles. The session does
    // NOT take ownership of any of these pointers.
    static std::unique_ptr<session> create(llama_context * lctx,
                                           const llama_model * model,
                                           mtmd_context  * ctx_mtmd,
                                           const session_params & params);

    virtual ~session() = default;

    // One-time bootstrap: tokenize and decode the system prompt.
    // Returns 0 on success, -1 on tokenization failure, -2 on decode failure.
    virtual int32_t start() = 0;

    // Process one frame. cur_rgb is tightly-packed RGB u8 of length nx*ny*3
    // (params.nx/ny). audio_text_optional is appended as a
    // "\n[Audio transcript]: ...\n" text chunk BEFORE the visual chunk and
    // persists across rewinds (matches MLX behaviour).
    virtual action process_frame(const unsigned char * cur_rgb,
                                 const std::string & audio_text_optional = {}) = 0;

    // Feed an EXTERNAL tool's result back into the session and resume the
    // per-frame agentic loop. Call this after process_frame() returns
    // ACTION_OTHER and the caller has executed the external tool. The current
    // frame's KV is kept (no rewind); only the tool-response turn + the model's
    // continued generation are appended. Returns the next action, which may be
    // another ACTION_OTHER (another tool round) or a terminal
    // speak/note/ignore/none. No-op before any frame: returns ACTION_NONE.
    virtual action continue_after_tool(const std::string & tool_result) = 0;

    // Direct rewind hook. Trims attention KV to the most recent pre-frame mark
    // and restores n_past. Recurrent state is intentionally NOT rolled back.
    // No-op if no frame has been processed yet.
    virtual void rewind_last() = 0;

    // Optional callbacks. Set to nullptr to skip. cb_user is opaque.
    virtual void set_speak_cb(speak_cb cb, void * cb_user) = 0;
    virtual void set_note_cb (note_cb  cb, void * cb_user) = 0;

    // Telemetry.
    virtual llama_pos n_past() const = 0;
    virtual int32_t   frame_count() const = 0;
    virtual size_t    cumulative_grid_size() const = 0;
    virtual uint32_t  cumulative_t_sum() const = 0; // M-RoPE invariant (test hook)
};

} // namespace vlib
