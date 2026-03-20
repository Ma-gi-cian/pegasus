#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_opengl3.h"
#include <SDL2/SDL.h>
#include <SDL2/SDL_opengl.h>

#include "cosim/PegasusCoSim.hpp"
#include "cosim/Event.hpp"


static const char* REG_NAMES[32] = {
        "zero", "ra", "sp",  "gp",  "tp",  "t0",  "t1", "t2",
    "s0",   "s1", "a0",  "a1",  "a2",  "a3",  "a4", "a5",
    "a6",   "a7", "s2",  "s3",  "s4",  "s5",  "s6", "s7",
    "s8",   "s9", "s10", "s11", "t3",  "t4",  "t5", "t6"
};

enum class Stage
{
    FETCH,
    DECODE,
    EXECUTE,
    COMMIT,
    DONE
};

static const char* get_name(Stage s)
{
    switch (s)
    {
        case Stage::FETCH:
            {
                return "FETCH";
            }
        case Stage::DECODE:
            {
                return "DECODE";
            }
        case Stage::EXECUTE:
            {
                return "EXECUTE";
            }
        case Stage::COMMIT:
            {
                return "COMMIT";
            }
        case Stage::DONE:
            {
                return "DONE";
            }
        default:
            {
                return "NONE";
            }
    }
}

static ImVec4 stageColor(Stage s)
{
    switch (s)
    {
        case Stage::FETCH:
            return {0.4f, 0.7f, 1.0f, 1.0f}; //  this is blue color
        case Stage::DECODE:
            return {1.0f, 0.8f, 0.2f, 1.0f}; // yellow
        case Stage::EXECUTE:
            return {1.0f, 0.5f, 0.2f, 1.0f}; // orange
        case Stage::COMMIT:
            return {0.4f, 1.0f, 0.4f, 1.0f}; // green
        case Stage::DONE:
            return {0.5f, 0.5f, 0.5f, 1.0f}; // grey
    }
    return {1, 1, 1, 1};
}

struct PipelineObject
{
    uint64_t    euid   = 0;
    uint64_t    pc     = 0;
    std::string dasm;
    uint32_t    opcode = 0;
    Stage       stage  = Stage::FETCH;
};

/**
Simulation state - this thing is responsible for storing the entire state of the simulation
*/

struct SimState
{
    pegasus::cosim::PegasusCoSim* cosim = nullptr;
    bool running = false;
    bool finished = false;
    bool step_automatically = false;

    std::string file_path;
    std::string config_path;
    uint64_t ilimit = 10000;

    std::map<std::string, std::string> params = {
        {"top.core0.params.isa", "rv64imafdcbv_zicsr_zifencei_zbkx"}};

    // I wish to store the values of all the registers some how
    // or else I will have to build that coding challenge question
    // where we are given a list of function calls and all and then we are told to give the data
    // when the function call ends and the other starts


    std::array<uint64_t, 32> reg_vals = {};
    std::array<uint64_t, 32> reg_vals_prev = {};
    std::array<bool, 32> reg_changed = {};

    std::vector<PipelineObject> pipeline;

    struct LogEntry
    {
        uint64_t euid;
        uint64_t pc;
        std::string dasm;
        uint32_t opcode;
        bool committed;
    };

    std::vector<LogEntry> logs;

    int total_step = 0;
    int total_commited = 0;
    bool error_occured = false;
    std::string status_msg;
};


static std::string hex64(uint64_t v)
{
    std::ostringstream ss;
    ss << "0x" << std::setw(16) << std::setfill('0') << std::hex << v;
    return ss.str();
}

static std::string hex32(uint32_t v)
{
    std::ostringstream ss;
    ss << "0x" << std::setw(8) << std::setfill('0') << std::hex << v;
    return ss.str();
}


static void refreshRegisters(SimState& s)
{
    /*
    This function goes and gets the register values every frame / every execution and then compares it with the register values before hand - and assigns true false to the boolean register changed so that we can do color changes to that particular row. 
    */
    s.reg_changed = {};
    for(int i = 0; i < 32; i++){
        std::vector<uint8_t> buf(8,0);
        // buff needs to be a vector because down the line a resize operation is being performed in it and well the function signature is that of vector so
        try{
            s.cosim->peekRegister(0,0, REG_NAMES[i], buf);
        } catch(...) { 
            continue;
        }

        uint64_t val = 0;
        for(int b = 0; b < 8 && b < (int)buf.size(); ++b){
            val |= (uint64_t)buf[b] << (8 * b);
        }

        s.reg_changed[i] = (val != s.reg_vals[i]);
        s.reg_vals_prev[i] = s.reg_vals[i];
        s.reg_vals[i] = val;
    }
}

static void advancePipeline(SimState& s, pegasus::cosim::EventAccessor& evt){
    for(auto& p: s.pipeline){
        switch(p.stage){
            case Stage::FETCH: {
                p.stage = Stage::DECODE;
                break;
            }
            case Stage::DECODE: {
                p.stage = Stage::EXECUTE;
                break;
            }
            case Stage::EXECUTE: {
                p.stage = Stage::COMMIT;
                break;
            }
            case Stage::COMMIT : {
                p.stage = Stage::DONE;
                break;
            }
            case Stage::DONE:{
                break;
            }
        }
    }

    const pegasus::cosim::Event* e = evt.get(); // getting the event data from the EventAccessor
    PipelineObject entry;
    entry.euid = evt.getEuid();
    entry.pc = e->getPc();
    entry.dasm = e->getDisassemblyStr();
    entry.opcode = e->getOpcode();
    s.pipeline.push_back(entry);
}

static void initialize(SimState& s)
{
    if (s.cosim != nullptr) return;
    if (s.running) return;
    try
    {
        s.cosim = new pegasus::cosim::PegasusCoSim(
            s.ilimit, s.file_path, s.params, "pegasus-cosim.db", 100
        );
        s.running = true;
        s.finished = false;
        s.total_step = 0;
        s.total_commited = 0;
        s.logs.clear();
        s.pipeline.clear();
        s.reg_vals = {};
        s.reg_vals_prev = {};
        s.reg_changed = {};
        refreshRegisters(s);
        s.status_msg = "Launched. Ready to step.";
        s.error_occured = false;
    }
    catch (const std::exception& e)
    {
        s.status_msg = std::string("Launch failed: ") + e.what();
        s.error_occured = true;
        s.running    = false;
        delete s.cosim;
        s.cosim = nullptr;
    }
}

static void walk(SimState & s)
{
    if (!s.running || s.finished)
        return;

    try
    {
        auto evt = s.cosim->step(
            0, 0); 
            // passing the core_id and hart_id - no override right now - I believe that step
            // is not returning any EventAccessor right now so not completely implemented.
            // Will send a PR after this and writing the Pegasus olympia feasibility document

        advancePipeline(s, evt);

        // evt has euid - the euid does not change it always remains the same - but the status of the event assosiated with the eventaccessor will change hence we can do the below thing.
        SimState::LogEntry entry;
        const pegasus::cosim::Event* e = evt.get();
        entry.euid = evt.getEuid(); // NOTE: USE DOT HERE - NO ARROW
        entry.pc = e->getPc();
        entry.dasm = e->getDisassemblyStr();
        entry.opcode = e->getOpcode();

        s.logs.push_back(entry);
        
        s.cosim->commit(evt);
        s.total_commited += 1;
        s.total_step += 1;

        refreshRegisters(s);
        s.finished = s.cosim->isSimulationFinished(0, 0);
        if(s.finished){
            s.status_msg = "Finished Execution after " +  std::to_string(s.total_step) + " steps";
            try{
                s.cosim->finish();
            } catch (...){

            }
        } else {
            s.status_msg = "Currently at PC : " + hex64(e->getPc()) + " " + e->getDisassemblyStr();
        }
        s.error_occured = false;
    } catch(std::exception& e){
        s.status_msg = std::string("ERROR: ") + e.what();
        s.step_automatically = false; // Incase the entire thing is running automatically and an error occurs - we dont want the simulator to keep on trying to run itself - we just wnat it to pause a little.
        s.error_occured = true;
    }
}

static void resetSim(SimState& s)
{
    if(s.cosim){
        try{
            s.cosim->finish();
        } catch(...){}
        delete s.cosim;
        s.cosim = nullptr;
    }
    s.running = false;
    s.finished = false;
    s.step_automatically = false;
    s.total_step = 0;
    s.total_commited = 0;
    s.logs.clear();
    s.pipeline.clear();
    s.reg_vals = {};
    s.reg_vals_prev = {};
    s.reg_changed = {};
    s.status_msg = "Reset.";
    s.error_occured = false;
}

// The above tool bar drawing
// currently I still need to add the method of taking a input in the header of the number of steps to walk and then walking the steps
// The logic is not hard - but gui is.
static void drawToolbar(SimState& s, float W, float H)
{
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove
                           | ImGuiWindowFlags_NoCollapse
                           | ImGuiWindowFlags_NoBringToFrontOnFocus
                           | ImGuiWindowFlags_NoTitleBar;

    ImGui::SetNextWindowPos({0, 0});
    ImGui::SetNextWindowSize({W, H});
    if (ImGui::Begin("##toolbar", nullptr, flags))
    {
        if (!s.running)
        {
            if (ImGui::Button("  Launch  ")) initialize(s);
        }
        else
        {
            ImGui::BeginDisabled(s.finished);
            if (ImGui::Button("  Step  "))          walk(s);
            ImGui::SameLine();
            ImGui::Checkbox("Auto Step", &s.step_automatically);
            ImGui::EndDisabled();

            ImGui::SameLine();
            if (ImGui::Button("  Reset  "))         resetSim(s);
        }

        ImGui::SameLine(0, 30);
        ImGui::TextDisabled("Steps: %d", s.total_step);

        ImGui::SameLine(0, 20);
        ImVec4 col = s.error_occured
            ? ImVec4(1.0f, 0.3f, 0.3f, 1.0f)
            : ImVec4(0.5f, 1.0f, 0.5f, 1.0f);
        ImGui::TextColored(col, "%s", s.status_msg.c_str());
    }
    ImGui::End();
}

static void drawRegisterFile(SimState& s, float x, float y, float W, float H){
        ImGuiWindowFlags flags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove
                           | ImGuiWindowFlags_NoCollapse
                           | ImGuiWindowFlags_NoBringToFrontOnFocus;

    ImGui::SetNextWindowPos({x, y});
    ImGui::SetNextWindowSize({W, H});
    if (ImGui::Begin("Register File", nullptr, flags))
    {
        if (ImGui::BeginTable("regfile", 3,
            ImGuiTableFlags_Borders     |
            ImGuiTableFlags_RowBg       |
            ImGuiTableFlags_ScrollY     |
            ImGuiTableFlags_SizingFixedFit,
            ImVec2(0, H - 40)))
        {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Reg",   ImGuiTableColumnFlags_WidthFixed,  50);
            ImGui::TableSetupColumn("Alias", ImGuiTableColumnFlags_WidthFixed,  50);
            ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();

            for (int i = 0; i < 32; ++i)
            {
                ImGui::TableNextRow();

                // The registers that will change will be yellow in color - got the color scheme from the pictures in the IDE readme in pegasus
                ImVec4 col;
                // This is zero - this register is forever low - its grey
                if (i == 0) col = {0.5f, 0.5f, 0.5f, 1.0f}; 
                // here are the changed registers colored in yellow
                else if (s.reg_changed[i]) col = {1.0f, 1.0f, 0.2f, 1.0f};  
                // Here are the unchanged registers colored in white
                else col = {0.9f, 0.9f, 0.9f, 1.0f};
  

                ImGui::TableSetColumnIndex(0);
                ImGui::TextColored(col, "x%d", i);

                ImGui::TableSetColumnIndex(1);
                ImGui::TextColored(col, "%s", REG_NAMES[i]);

                ImGui::TableSetColumnIndex(2);
                ImGui::TextColored(col, "%s", hex64(s.reg_vals[i]).c_str());
            }
            ImGui::EndTable();
        }
    }
    ImGui::End();
}

// Pipeline view — shows recent instructions and which stage they are in
static void drawPipelineView(SimState& s, float x, float y, float W, float H)
{
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove
                           | ImGuiWindowFlags_NoCollapse
                           | ImGuiWindowFlags_NoBringToFrontOnFocus;

    ImGui::SetNextWindowPos({x, y});
    ImGui::SetNextWindowSize({W, H});
    if (ImGui::Begin("Pipeline", nullptr, flags))
    {
        if (ImGui::BeginTable("pipeline", 4,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit))
        {
            ImGui::TableSetupColumn("Stage",  ImGuiTableColumnFlags_WidthFixed,  80);
            ImGui::TableSetupColumn("PC",     ImGuiTableColumnFlags_WidthFixed, 160);
            ImGui::TableSetupColumn("Opcode", ImGuiTableColumnFlags_WidthFixed,  90);
            ImGui::TableSetupColumn("Disasm", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();

            for (auto it = s.pipeline.rbegin(); it != s.pipeline.rend(); ++it)
            {
                const auto& p = *it;
                ImVec4 col = stageColor(p.stage);
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextColored(col, "%s", get_name(p.stage));
                ImGui::TableSetColumnIndex(1);
                ImGui::TextColored(col, "%s", hex64(p.pc).c_str());
                ImGui::TableSetColumnIndex(2);
                ImGui::TextColored(col, "%s", hex32(p.opcode).c_str());
                ImGui::TableSetColumnIndex(3);
                ImGui::TextColored(col, "%s", p.dasm.c_str());
            }
            ImGui::EndTable();
        }
    }
    ImGui::End();
}

// Event log — all the instructions are visible here -- well they are also visible in the pipeline - but this area is bigger to view 
static void drawEventLog(SimState& s, float x, float y, float W, float H)
{
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove
                           | ImGuiWindowFlags_NoCollapse
                           | ImGuiWindowFlags_NoBringToFrontOnFocus;

    ImGui::SetNextWindowPos({x, y});
    ImGui::SetNextWindowSize({W, H});
    if (ImGui::Begin("Instruction Log", nullptr, flags))
    {
        if (ImGui::BeginTable("evtlog", 4,
            ImGuiTableFlags_Borders       |
            ImGuiTableFlags_RowBg         |
            ImGuiTableFlags_ScrollY       |
            ImGuiTableFlags_SizingFixedFit,
            ImVec2(0, H - 40)))
        {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("EUID",   ImGuiTableColumnFlags_WidthFixed,   70);
            ImGui::TableSetupColumn("PC",     ImGuiTableColumnFlags_WidthFixed,  160);
            ImGui::TableSetupColumn("Opcode", ImGuiTableColumnFlags_WidthFixed,   90);
            ImGui::TableSetupColumn("Disasm", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();

            for (auto it = s.logs.rbegin(); it != s.logs.rend(); ++it)
            {
                const auto& e = *it;
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::Text("%llu", (unsigned long long)e.euid);
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%s", hex64(e.pc).c_str());
                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%s", hex32(e.opcode).c_str());
                ImGui::TableSetColumnIndex(3);
                ImGui::TextUnformatted(e.dasm.c_str());
            }
            ImGui::EndTable();
        }
    }
    ImGui::End();
}

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        fprintf(stderr, "usage: pegasus_debugger <elf_path> [-p key value ...]\n");
        return 1;
    }

    SimState state;
    state.file_path = argv[1];

    for (int i = 2; i < argc; ++i)
        if (std::string(argv[i]) == "-p" && i + 2 < argc)
        {
            state.params[argv[i+1]] = argv[i+2];
            i += 2;
        }

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0)
    {
        fprintf(stderr, "SDL_Init error: %s\n", SDL_GetError());
        return -1;
    }

    const char* glsl_version = "#version 330";
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,  SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE,   24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);

    SDL_Window* window = SDL_CreateWindow(
        "Pegasus CoSim Debugger",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1600, 900,
        (SDL_WindowFlags)(SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI)
    );
    if (!window) { fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError()); SDL_Quit(); return -1; }

    SDL_GLContext gl_ctx = SDL_GL_CreateContext(window);
    if (!gl_ctx) { fprintf(stderr, "SDL_GL_CreateContext: %s\n", SDL_GetError()); SDL_DestroyWindow(window); SDL_Quit(); return -1; }
    SDL_GL_MakeCurrent(window, gl_ctx);
    SDL_GL_SetSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();

    ImGui_ImplSDL2_InitForOpenGL(window, gl_ctx);
    ImGui_ImplOpenGL3_Init(glsl_version);

    bool running = true;
    while (running)
    {
        SDL_Event event;
        while (SDL_PollEvent(&event))
        {
            ImGui_ImplSDL2_ProcessEvent(&event);
            if (event.type == SDL_QUIT) running = false;
            if (event.type == SDL_WINDOWEVENT &&
                event.window.event == SDL_WINDOWEVENT_CLOSE &&
                event.window.windowID == SDL_GetWindowID(window))
                running = false;
        }

        if (state.step_automatically && state.running && !state.finished) walk(state);

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        const float W         = io.DisplaySize.x;
        const float H         = io.DisplaySize.y;
        const float TOOLBAR_H = 44.0f;
        const float TOP_H     = 320.0f;
        const float REG_W     = 300.0f;
        const float LOG_H     = H - TOOLBAR_H - TOP_H;

        drawToolbar     (state, W, TOOLBAR_H);
        drawRegisterFile(state, 0,     TOOLBAR_H,       REG_W,     TOP_H);
        drawPipelineView(state, REG_W, TOOLBAR_H,       W - REG_W, TOP_H);
        drawEventLog    (state, 0,     TOOLBAR_H+TOP_H, W,         LOG_H);

        ImGui::Render();
        glViewport(0, 0, (int)io.DisplaySize.x, (int)io.DisplaySize.y);
        glClearColor(0.08f, 0.08f, 0.08f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        SDL_GL_SwapWindow(window);
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_GL_DeleteContext(gl_ctx);
    SDL_DestroyWindow(window);
    SDL_Quit();

    return 0;
}