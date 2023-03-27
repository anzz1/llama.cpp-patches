#include "common.h"
#include "llama.h"

#include <cassert>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#if defined (__unix__) || (defined (__APPLE__) && defined (__MACH__))
#include <signal.h>
#include <unistd.h>
#elif defined (_WIN32)
#include <signal.h>
#endif

#if defined (_WIN32)
#pragma comment(lib,"kernel32.lib")
extern "C" __declspec(dllimport) void* __stdcall GetStdHandle(unsigned long nStdHandle);
extern "C" __declspec(dllimport) int __stdcall GetConsoleMode(void* hConsoleHandle, unsigned long* lpMode);
extern "C" __declspec(dllimport) int __stdcall SetConsoleMode(void* hConsoleHandle, unsigned long dwMode);
extern "C" __declspec(dllimport) int __stdcall SetConsoleCP(unsigned int wCodePageID);
extern "C" __declspec(dllimport) int __stdcall SetConsoleOutputCP(unsigned int wCodePageID);
#endif

#define ANSI_COLOR_RED     "\x1b[31m"
#define ANSI_COLOR_GREEN   "\x1b[32m"
#define ANSI_COLOR_YELLOW  "\x1b[33m"
#define ANSI_COLOR_BLUE    "\x1b[34m"
#define ANSI_COLOR_MAGENTA "\x1b[35m"
#define ANSI_COLOR_CYAN    "\x1b[36m"
#define ANSI_COLOR_RESET   "\x1b[0m"
#define ANSI_BOLD          "\x1b[1m"

/* Keep track of current color of output, and emit ANSI code if it changes. */
enum console_state {
    CONSOLE_STATE_DEFAULT=0,
    CONSOLE_STATE_PROMPT,
    CONSOLE_STATE_USER_INPUT
};

static console_state con_st = CONSOLE_STATE_DEFAULT;
static bool con_use_color = false;

void set_console_state(console_state new_st) {
    if (!con_use_color) return;
    // only emit color code if state changed
    if (new_st != con_st) {
        con_st = new_st;
        switch(con_st) {
        case CONSOLE_STATE_DEFAULT:
            printf(ANSI_COLOR_RESET);
            return;
        case CONSOLE_STATE_PROMPT:
            printf(ANSI_COLOR_YELLOW);
            return;
        case CONSOLE_STATE_USER_INPUT:
            printf(ANSI_BOLD ANSI_COLOR_GREEN);
            return;
        }
    }
}

static bool is_interacting = true;

#if defined (__unix__) || (defined (__APPLE__) && defined (__MACH__)) || defined (_WIN32)
void sigint_handler(int signo) {
    set_console_state(CONSOLE_STATE_DEFAULT);
    printf("\n"); // this also force flush stdout.
    if (signo == SIGINT) {
        if (!is_interacting) {
            is_interacting=true;
        } else {
            _exit(130);
        }
    }
}
#endif

#if defined (_WIN32)
void win32_console_init(void) {
    unsigned long dwMode = 0;
    void* hConOut = GetStdHandle((unsigned long)-11); // STD_OUTPUT_HANDLE (-11)
    if (!hConOut || hConOut == (void*)-1 || !GetConsoleMode(hConOut, &dwMode)) {
        hConOut = GetStdHandle((unsigned long)-12); // STD_ERROR_HANDLE (-12)
        if (hConOut && (hConOut == (void*)-1 || !GetConsoleMode(hConOut, &dwMode))) {
            hConOut = 0;
        }
    }
    if (hConOut) {
        // Enable ANSI colors on Windows 10+
        if (con_use_color && !(dwMode & 0x4)) {
            SetConsoleMode(hConOut, dwMode | 0x4); // ENABLE_VIRTUAL_TERMINAL_PROCESSING (0x4)
        }
        // Set console output codepage to UTF8
        SetConsoleOutputCP(65001); // CP_UTF8
    }
    void* hConIn = GetStdHandle((unsigned long)-10); // STD_INPUT_HANDLE (-10)
    if (hConIn && hConIn != (void*)-1 && GetConsoleMode(hConIn, &dwMode)) {
        // Set console input codepage to UTF8
        SetConsoleCP(65001); // CP_UTF8
    }
}
#endif

int main(int argc, char ** argv) {
    gpt_params params;
    params.model = "models/llama-7B/ggml-model.bin";

    if (gpt_params_parse(argc, argv, params) == false) {
        return 1;
    }

    // save choice to use color for later
    // (note for later: this is a slightly awkward choice)
    con_use_color = params.use_color;

#if defined (_WIN32)
    win32_console_init();
#endif

    if (params.perplexity) {
        printf("\n************\n");
        printf("%s: please use the 'perplexity' tool for perplexity calculations\n", __func__);
        printf("************\n\n");

        return 0;
    }

    if (params.embedding) {
        printf("\n************\n");
        printf("%s: please use the 'embedding' tool for embedding calculations\n", __func__);
        printf("************\n\n");

        return 0;
    }

    if (params.n_ctx > 2048) {
        fprintf(stderr, "%s: warning: model does not support context sizes greater than 2048 tokens (%d specified);"
                "expect poor results\n", __func__, params.n_ctx);
    }

    if (params.seed <= 0) {
        params.seed = time(NULL);
    }

    fprintf(stderr, "%s: seed = %d\n", __func__, params.seed);

    std::mt19937 rng(params.seed);
    if (params.random_prompt) {
        params.prompt = gpt_random_prompt(rng);
    }

    llama_context * ctx;

    // load the model
    {
        auto lparams = llama_context_default_params();

        lparams.n_ctx      = params.n_ctx;
        lparams.n_parts    = params.n_parts;
        lparams.seed       = params.seed;
        lparams.f16_kv     = params.memory_f16;
        lparams.use_mlock  = params.use_mlock;

        ctx = llama_init_from_file(params.model.c_str(), lparams);

        if (ctx == NULL) {
            fprintf(stderr, "%s: error: failed to load model '%s'\n", __func__, params.model.c_str());
            return 1;
        }
    }

    // print system information
    {
        fprintf(stderr, "\n");
        fprintf(stderr, "system_info: n_threads = %d / %d | %s\n",
                params.n_threads, std::thread::hardware_concurrency(), llama_print_system_info());
    }

    // determine the maximum memory usage needed to do inference for the given n_batch and n_predict parameters
    // uncomment the "used_mem" line in llama.cpp to see the results
    if (params.mem_test) {
        {
            const std::vector<llama_token> tmp(params.n_batch, 0);
            llama_eval(ctx, tmp.data(), tmp.size(), 0, params.n_threads);
        }

        {
            const std::vector<llama_token> tmp = { 0, };
            llama_eval(ctx, tmp.data(), tmp.size(), params.n_predict - 1, params.n_threads);
        }

        llama_print_timings(ctx);
        llama_free(ctx);

        return 0;
    }

    // Add a space in front of the first character to match OG llama tokenizer behavior
    params.prompt.insert(0, 1, ' ');

    // tokenize the prompt
    auto embd_inp = ::llama_tokenize(ctx, params.prompt, true);

    const int n_ctx = llama_n_ctx(ctx);

    if ((int) embd_inp.size() > n_ctx - 4) {
        fprintf(stderr, "%s: error: prompt is too long (%d tokens, max %d)\n", __func__, (int) embd_inp.size(), n_ctx - 4);
        return 1;
    }

    // always keep the prompt in instruct mode
    params.n_keep      = (int)embd_inp.size(); 

    // in instruct mode, we inject a prefix and a suffix to each input by the user
    const auto inp_pfx = ::llama_tokenize(ctx, "\n\n### Instruction:\n\n", true);
    const auto inp_sfx = ::llama_tokenize(ctx, "\n\n### Response:\n\n", false);
    params.antiprompt.push_back("### Instruction:\n\n");

    // determine newline token
    auto llama_token_newline = ::llama_tokenize(ctx, "\n", false);

    if (params.verbose_prompt) {
        fprintf(stderr, "\n");
        fprintf(stderr, "%s: prompt: '%s'\n", __func__, params.prompt.c_str());
        fprintf(stderr, "%s: number of tokens in prompt = %zu\n", __func__, embd_inp.size());
        for (int i = 0; i < (int) embd_inp.size(); i++) {
            fprintf(stderr, "%6d -> '%s'\n", embd_inp[i], llama_token_to_str(ctx, embd_inp[i]));
        }
        fprintf(stderr, "%s: static prompt based on n_keep: '", __func__);
        for (int i = 0; i < params.n_keep; i++) {
            fprintf(stderr, "%s", llama_token_to_str(ctx, embd_inp[i]));
        }
        fprintf(stderr, "'\n\n");
    }

#if defined (__unix__) || (defined (__APPLE__) && defined (__MACH__))
    struct sigaction sigint_action;
    sigint_action.sa_handler = sigint_handler;
    sigemptyset (&sigint_action.sa_mask);
    sigint_action.sa_flags = 0;
    sigaction(SIGINT, &sigint_action, NULL);
#elif defined (_WIN32)
    signal(SIGINT, sigint_handler);
#endif

    fprintf(stderr, "%s: interactive mode on.\n", __func__);

    for (auto antiprompt : params.antiprompt) {
        fprintf(stderr, "Reverse prompt: '%s'\n", antiprompt.c_str());
    }

    if (!params.input_prefix.empty()) {
        fprintf(stderr, "Input prefix: '%s'\n", params.input_prefix.c_str());
    }

    fprintf(stderr, "sampling: temp = %f, top_k = %d, top_p = %f, repeat_last_n = %i, repeat_penalty = %f\n", params.temp, params.top_k, params.top_p, params.repeat_last_n, params.repeat_penalty);
    fprintf(stderr, "generate: n_ctx = %d, n_batch = %d, n_predict = %d, n_keep = %d\n", n_ctx, params.n_batch, params.n_predict, params.n_keep);
    fprintf(stderr, "\n\n");

    // TODO: replace with ring-buffer
    std::vector<llama_token> last_n_tokens(n_ctx);
    std::fill(last_n_tokens.begin(), last_n_tokens.end(), 0);

    fprintf(stderr, "== Running in interactive mode. ==\n"
#if defined (__unix__) || (defined (__APPLE__) && defined (__MACH__)) || defined (_WIN32)
           " - Press Ctrl+C to interject at any time.\n"
#endif
           " - Press Return to return control to LLaMa.\n"
           " - If you want to submit another line, end your input in '\\'.\n\n");

    bool input_noecho = false;

    int n_past     = 0;
    int n_remain   = params.n_predict;
    int n_consumed = 0;

    // the first thing we will do is to output the prompt, so set color accordingly
    set_console_state(CONSOLE_STATE_PROMPT);

    std::vector<llama_token> embd;

    while (1) {
        // predict
        if (embd.size() > 0) {
            // infinite text generation via context swapping
            // if we run out of context:
            // - take the n_keep first tokens from the original prompt (via n_past)
            // - take half of the last (n_ctx - n_keep) tokens and recompute the logits in a batch
            if (n_past + (int) embd.size() > n_ctx) {
                const int n_left = n_past - params.n_keep;

                n_past = params.n_keep;

                // insert n_left/2 tokens at the start of embd from last_n_tokens
                embd.insert(embd.begin(), last_n_tokens.begin() + n_ctx - n_left/2 - embd.size(), last_n_tokens.end() - embd.size());

                printf("\n---\n");
                printf("resetting: '");
                for (int i = 0; i < (int) embd.size(); i++) {
                    printf("%s", llama_token_to_str(ctx, embd[i]));
                }
                printf("'\n");
                printf("\n---\n");
            }

            if (llama_eval(ctx, embd.data(), embd.size(), n_past, params.n_threads)) {
                fprintf(stderr, "%s : failed to eval\n", __func__);
                return 1;
            }
        }

        n_past += embd.size();
        embd.clear();

        if ((int) embd_inp.size() <= n_consumed && !is_interacting) {
            // out of user input, sample next token
            const float top_k          = params.top_k;
            const float top_p          = params.top_p;
            const float temp           = params.temp;
            const float repeat_penalty = params.repeat_penalty;

            llama_token id = 0;

            {
                auto logits = llama_get_logits(ctx);

                if (params.ignore_eos) {
                    logits[llama_token_eos()] = 0;
                }

                id = llama_sample_top_p_top_k(ctx,
                        last_n_tokens.data() + n_ctx - params.repeat_last_n,
                        params.repeat_last_n, top_k, top_p, temp, repeat_penalty);

                last_n_tokens.erase(last_n_tokens.begin());
                last_n_tokens.push_back(id);
            }

            // add it to the context
            embd.push_back(id);

            // echo this to console
            input_noecho = false;

            // decrement remaining sampling budget
            --n_remain;
        } else {
            // some user input remains from prompt or interaction, forward it to processing
            while ((int) embd_inp.size() > n_consumed) {
                embd.push_back(embd_inp[n_consumed]);
                last_n_tokens.erase(last_n_tokens.begin());
                last_n_tokens.push_back(embd_inp[n_consumed]);
                ++n_consumed;
                if ((int) embd.size() >= params.n_batch) {
                    break;
                }
            }
        }

        // display text
        if (!input_noecho) {
            for (auto id : embd) {
                printf("%s", llama_token_to_str(ctx, id));
            }
            fflush(stdout);
        }
        // reset color to default if we there is no pending user input
        if (!input_noecho && (int)embd_inp.size() == n_consumed) {
            set_console_state(CONSOLE_STATE_DEFAULT);
        }

        // in interactive mode, and not currently processing queued inputs;
        // check if we should prompt the user for more
        if (params.interactive && (int) embd_inp.size() <= n_consumed) {
            // check for reverse prompt
            std::string last_output;
            for (auto id : last_n_tokens) {
                last_output += llama_token_to_str(ctx, id);
            }

            // Check if each of the reverse prompts appears at the end of the output.
            for (std::string & antiprompt : params.antiprompt) {
                if (last_output.find(antiprompt.c_str(), last_output.length() - antiprompt.length(), antiprompt.length()) != std::string::npos) {
                    is_interacting = true;
                    set_console_state(CONSOLE_STATE_USER_INPUT);
                    fflush(stdout);
                    break;
                }
            }

            if (n_past > 0 && is_interacting) {
                // consume, consume
                n_consumed = embd_inp.size();

                // potentially set color to indicate we are taking user input
                set_console_state(CONSOLE_STATE_USER_INPUT);

                printf("\n> ");

                std::string buffer;
                if (!params.input_prefix.empty()) {
                    buffer += params.input_prefix;
                    printf("%s", buffer.c_str());
                }

                std::string line;
                bool another_line = true;
                do {
                    if (!std::getline(std::cin, line)) {
                        // input stream is bad or EOF received
                        return 0;
                    }
                    if (line.empty() || line.back() != '\\') {
                        another_line = false;
                    } else {
                        line.pop_back(); // Remove the continue character
                    }
                    buffer += line + '\n'; // Append the line to the result
                } while (another_line);

                // done taking input, reset color
                set_console_state(CONSOLE_STATE_DEFAULT);

                // Add tokens to buffer only if the line is non-empty.
                if (buffer.length() > 1) {
                    // insert instruction prefix
                    embd_inp.insert(embd_inp.end(), inp_pfx.begin(), inp_pfx.end());

                    auto line_inp = ::llama_tokenize(ctx, buffer, false);
                    embd_inp.insert(embd_inp.end(), line_inp.begin(), line_inp.end());

                    // insert response suffix
                    embd_inp.insert(embd_inp.end(), inp_sfx.begin(), inp_sfx.end());

                    n_remain -= line_inp.size();
                }

                input_noecho = true; // do not echo this again
            }

            if (n_past > 0) {
                is_interacting = false;
            }
        }

        // end of text token
        if (embd.back() == llama_token_eos()) {
            is_interacting = true;
        }

        // In interactive mode, respect the maximum number of tokens and drop back to user input when reached.
        if (n_remain <= 0 && params.n_predict != -1) {
            n_remain = params.n_predict;
            is_interacting = true;
        }
    }

#if defined (_WIN32)
    signal(SIGINT, SIG_DFL);
#endif

    llama_print_timings(ctx);
    llama_free(ctx);

    set_console_state(CONSOLE_STATE_DEFAULT);

    return 0;
}
