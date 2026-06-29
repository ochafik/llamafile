// mtmd_video_cli.cpp — browser-free harness for the vlib-video continuous
// session. Folder-of-JPEGs (sorted) -> run the session step-by-step -> print
// the action / tool-call the model emits per frame.
//
//   llamafile mtmd-video-cli -m MODEL.gguf --mmproj MMPROJ.gguf \
//       --frames DIR [-p "watch goal"] [--frame-size 448] [--n-ctx 8192]
//
// Dispatched as an early subcommand from main.cpp (like `wikipedia` /
// `mcp-server`), so it short-circuits the server/chatbot init. It does its own
// GPU + backend bring-up and loads model+mmproj directly via the public APIs.
//
// This is the deterministic, no-browser oracle for the webcam-agent server work
// (step 2): /agent SSE endpoints will drive the same vlib::session.

#include <cosmo.h>
#include <dirent.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "common.h"
#include "llama.h"
#include "log.h"
#include "mtmd.h"
#include "mtmd-helper.h"

#include "llamafile.h"
#include "vlib_video/vlib_video_session.h"

#include "stb/stb_image.h"
#include "stb/stb_image_resize2.h"

namespace {

void usage(const char * argv0) {
    fprintf(stderr,
        "usage: %s mtmd-video-cli -m MODEL.gguf --mmproj MMPROJ.gguf --frames DIR\n"
        "                         [-p PROMPT] [--frame-size N] [--n-ctx N]\n"
        "                         [-ngl N] [--max-tool-tokens N] [--verbose]\n",
        argv0);
}

bool has_suffix_ci(const std::string & s, const char * suf) {
    size_t n = strlen(suf);
    if (s.size() < n) return false;
    for (size_t i = 0; i < n; ++i) {
        if (std::tolower((unsigned char) s[s.size() - n + i]) != std::tolower((unsigned char) suf[i]))
            return false;
    }
    return true;
}

// Sorted list of image files in dir (jpg/jpeg/png).
std::vector<std::string> list_frames(const std::string & dir) {
    std::vector<std::string> out;
    DIR * d = opendir(dir.c_str());
    if (!d) return out;
    struct dirent * e;
    while ((e = readdir(d)) != nullptr) {
        std::string name = e->d_name;
        if (name == "." || name == "..") continue;
        if (has_suffix_ci(name, ".jpg") || has_suffix_ci(name, ".jpeg") ||
            has_suffix_ci(name, ".png")) {
            out.push_back(dir + "/" + name);
        }
    }
    closedir(d);
    std::sort(out.begin(), out.end());
    return out;
}

// Load an image, resize to size*size RGB u8 (tightly packed). Returns empty on
// failure.
std::vector<unsigned char> load_frame_rgb(const std::string & path, int size) {
    int nx = 0, ny = 0, comp = 0;
    unsigned char * pixels = stbi_load(path.c_str(), &nx, &ny, &comp, 3);
    if (!pixels) return {};
    std::vector<unsigned char> out((size_t) size * size * 3);
    int rc = (int) (intptr_t) stbir_resize_uint8_srgb(
        pixels, nx, ny, 0,
        out.data(), size, size, 0,
        (stbir_pixel_layout) STBIR_RGB);
    stbi_image_free(pixels);
    if (rc == 0) return {};
    return out;
}

std::string build_system_prompt(const std::string & goal) {
    std::string g = goal.empty()
        ? "Watch the scene and report meaningful changes."
        : goal;
    return
        "<|im_start|>system\n"
        "You are a real-time video-watching agent. You are shown frames one at a "
        "time; each frame is encoded together with the last frame you reacted to, "
        "so you effectively see what changed. After each frame respond with "
        "EXACTLY ONE tool call wrapped in <tool_call>...</tool_call> as JSON: "
        "{\"name\": <tool>, \"arguments\": {...}}.\n"
        "Available tools:\n"
        "- ignore_frame: nothing noteworthy changed. arguments: {}\n"
        "- note: record a brief observation for later. arguments: {\"observation\": \"...\"}\n"
        "- speak: say something out loud to the user right now. arguments: {\"text\": \"...\"}\n"
        "Watch goal: " + g + "\n"
        "You MUST wrap the call exactly like <tool_call>{\"name\": ..., \"arguments\": {...}}</tool_call>. "
        "Do not write any other text and do not think out loud. /no_think<|im_end|>\n";
}

} // namespace

int mtmd_video_cli_main(int argc, char ** argv) {
    std::string model_path, mmproj_path, frames_dir, prompt;
    int frame_size = 448;
    int n_ctx = 8192;
    int ngl = -1;
    int max_tool_tokens = 128;
    bool verbose = false;

    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](const char * name) -> const char * {
            if (i + 1 >= argc) { fprintf(stderr, "error: %s needs a value\n", name); exit(2); }
            return argv[++i];
        };
        if (a == "-m" || a == "--model")            model_path  = next("-m");
        else if (a == "--mmproj")                   mmproj_path = next("--mmproj");
        else if (a == "--frames" || a == "--frames-dir") frames_dir = next("--frames");
        else if (a == "-p" || a == "--prompt")      prompt      = next("-p");
        else if (a == "--frame-size")               frame_size  = atoi(next("--frame-size"));
        else if (a == "--n-ctx" || a == "-c")       n_ctx       = atoi(next("--n-ctx"));
        else if (a == "-ngl" || a == "--n-gpu-layers") ngl      = atoi(next("-ngl"));
        else if (a == "--max-tool-tokens")          max_tool_tokens = atoi(next("--max-tool-tokens"));
        else if (a == "--verbose" || a == "-v")     verbose = true;
        else if (a == "-h" || a == "--help")        { usage(argv[0]); return 0; }
        else { fprintf(stderr, "error: unknown arg '%s'\n", a.c_str()); usage(argv[0]); return 2; }
    }

    if (model_path.empty() || mmproj_path.empty() || frames_dir.empty()) {
        fprintf(stderr, "error: -m, --mmproj and --frames are required\n");
        usage(argv[0]);
        return 2;
    }
    // Round frame size to a multiple of patch_size*spatial_merge (28) so the
    // grid math is clean and ref/cur preprocess identically.
    if (frame_size % 28 != 0) frame_size = (frame_size / 28) * 28;
    if (frame_size < 28) frame_size = 28;

    // GPU + backend bring-up (we dispatched before main.cpp did this).
    llamafile_early_gpu_init(argv);
    llamafile_has_gpu();
    if (!verbose) {
        llama_log_set((ggml_log_callback) llamafile_log_callback_null, nullptr);
        llamafile_metal_log_set(llamafile_log_callback_null, nullptr);
        llamafile_cuda_log_set(llamafile_log_callback_null, nullptr);
        mtmd_helper_log_set((ggml_log_callback) llamafile_log_callback_null, nullptr);
    }
    llama_backend_init();
    common_init();

    if (ngl < 0 && llamafile_has_metal()) ngl = INT_MAX; // offload all on Metal

    // ---- load model ----
    common_params params;
    params.model.path  = model_path;
    params.mmproj.path = mmproj_path;
    params.n_ctx       = n_ctx;
    params.n_batch     = 512;
    params.n_gpu_layers = ngl;

    fprintf(stderr, "loading model: %s\n", model_path.c_str());
    llama_model_params mp = common_model_params_to_llama(params);
    llama_model * model = llama_model_load_from_file(model_path.c_str(), mp);
    if (!model) { fprintf(stderr, "error: failed to load model\n"); return 3; }

    if (params.n_ctx <= 0 || params.n_ctx > (int) llama_model_n_ctx_train(model))
        params.n_ctx = llama_model_n_ctx_train(model);

    llama_context_params cp = common_context_params_to_llama(params);
    llama_context * ctx = llama_init_from_model(model, cp);
    if (!ctx) { fprintf(stderr, "error: failed to create context\n"); llama_model_free(model); return 3; }

    // ---- load mmproj / mtmd ----
    fprintf(stderr, "loading vision projector: %s\n", mmproj_path.c_str());
    mtmd_context_params mparams = mtmd_context_params_default();
    mparams.use_gpu       = (ngl != 0);
    mparams.print_timings = verbose;
    mparams.n_threads     = params.cpuparams.n_threads;
    mtmd_context * mctx = mtmd_init_from_file(mmproj_path.c_str(), model, mparams);
    if (!mctx) { fprintf(stderr, "error: failed to init mmproj\n"); return 4; }

    // ---- frames ----
    std::vector<std::string> frames = list_frames(frames_dir);
    if (frames.empty()) {
        fprintf(stderr, "error: no .jpg/.jpeg/.png frames in %s\n", frames_dir.c_str());
        return 5;
    }
    fprintf(stderr, "found %zu frame(s) in %s, frame_size=%d\n",
            frames.size(), frames_dir.c_str(), frame_size);

    // ---- session ----
    vlib::session_params sp;
    sp.nx = (uint32_t) frame_size;
    sp.ny = (uint32_t) frame_size;
    sp.n_batch = params.n_batch;
    sp.max_tool_tokens = max_tool_tokens;
    sp.system_prompt = build_system_prompt(prompt);

    auto sess = vlib::session::create(ctx, model, mctx, sp);
    sess->set_speak_cb([](const std::string & text, void *) {
        printf("    >> SPEAK: %s\n", text.c_str());
    }, nullptr);
    sess->set_note_cb([](const std::string & obs, void *) {
        printf("    >> NOTE:  %s\n", obs.c_str());
    }, nullptr);

    if (sess->start() != 0) {
        fprintf(stderr, "error: session start (system prompt decode) failed\n");
        return 6;
    }

    static const char * kind_name[] = { "NONE", "IGNORE_FRAME", "SPEAK", "NOTE", "OTHER" };

    printf("\n=== running session over %zu frames ===\n", frames.size());
    for (size_t i = 0; i < frames.size(); ++i) {
        std::vector<unsigned char> rgb = load_frame_rgb(frames[i], frame_size);
        if (rgb.empty()) {
            fprintf(stderr, "warning: skipping unreadable frame %s\n", frames[i].c_str());
            continue;
        }
        vlib::action act = sess->process_frame(rgb.data());
        const char * kn = (act.kind >= 0 && act.kind <= 4) ? kind_name[act.kind] : "?";
        printf("[frame %zu] %s  action=%s  n_past=%d  grid_t=%u\n",
               i, frames[i].c_str(), kn, (int) sess->n_past(), sess->cumulative_t_sum());
        if (!act.call.name.empty()) {
            printf("    tool_call: %s {", act.call.name.c_str());
            bool first = true;
            for (auto & kv : act.call.arguments) {
                printf("%s%s=%s", first ? "" : ", ", kv.first.c_str(), kv.second.c_str());
                first = false;
            }
            printf("}\n");
        }
        std::string raw = act.raw_assistant_text;
        if (raw.size() > 200) raw = raw.substr(0, 200) + "...";
        printf("    raw: %s\n", raw.c_str());
    }
    printf("=== done: %d frames processed, final n_past=%d, cumulative grid frames=%zu ===\n",
           sess->frame_count(), (int) sess->n_past(), sess->cumulative_grid_size());

    sess.reset();
    mtmd_free(mctx);
    llama_free(ctx);
    llama_model_free(model);
    return 0;
}
