// P3 prototype: minimal MCP stdio client under cosmocc.
// Spawns an MCP server (argv[1..]) via posix_spawn, talks newline JSON-RPC 2.0.
#include <nlohmann/json.hpp>
#include <spawn.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/wait.h>

using json = nlohmann::ordered_json;
extern char **environ;

static FILE *g_in;   // write to child's stdin
static FILE *g_out;  // read from child's stdout
static int g_id = 0;

static json rpc_call(const std::string &method, json params, bool notify=false) {
    json msg = {{"jsonrpc","2.0"},{"method",method}};
    if (!params.is_null()) msg["params"] = params;
    if (!notify) msg["id"] = ++g_id;
    std::string line = msg.dump() + "\n";
    fwrite(line.data(), 1, line.size(), g_in); fflush(g_in);
    if (notify) return json();
    // read one response line
    char buf[1<<16];
    if (!fgets(buf, sizeof buf, g_out)) { fprintf(stderr,"no response to %s\n",method.c_str()); return json(); }
    return json::parse(buf, nullptr, false);
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr,"usage: %s server-cmd [args...]\n",argv[0]); return 2; }
    int c_in[2], c_out[2];
    if (pipe2(c_in,0) || pipe2(c_out,0)) { perror("pipe2"); return 1; }

    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, c_in[0], STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&fa, c_out[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&fa, c_in[1]);
    posix_spawn_file_actions_addclose(&fa, c_out[0]);

    pid_t pid;
    if (posix_spawnp(&pid, argv[1], &fa, nullptr, &argv[1], environ) != 0) {
        perror("posix_spawnp"); return 1;
    }
    close(c_in[0]); close(c_out[1]);
    g_in  = fdopen(c_in[1], "w");
    g_out = fdopen(c_out[0], "r");

    printf("== initialize ==\n");
    json init = rpc_call("initialize", json{{"protocolVersion","2025-06-18"},
        {"capabilities",json::object()},{"clientInfo",{{"name","llamafile"},{"version","1.0"}}}});
    printf("  server=%s protocol=%s\n",
        init["result"]["serverInfo"]["name"].dump().c_str(),
        init["result"]["protocolVersion"].dump().c_str());

    rpc_call("notifications/initialized", json::object(), /*notify=*/true);

    printf("== tools/list ==\n");
    json tl = rpc_call("tools/list", json(nullptr));
    for (auto &t : tl["result"]["tools"])
        printf("  tool: %s — %s\n", t["name"].get<std::string>().c_str(),
               t["description"].get<std::string>().c_str());

    printf("== tools/call echo ==\n");
    json tc = rpc_call("tools/call", json{{"name","echo"},{"arguments",{{"text","hello from cosmocc MCP client"}}}});
    printf("  result: %s\n", tc["result"]["content"][0]["text"].get<std::string>().c_str());

    fclose(g_in);  // EOF -> server exits
    int st=0; waitpid(pid,&st,0);
    posix_spawn_file_actions_destroy(&fa);
    printf("== OK (server exit=%d) ==\n", WIFEXITED(st)?WEXITSTATUS(st):-1);
    return 0;
}
