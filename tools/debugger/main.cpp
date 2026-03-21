#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_opengl3.h"
#include <SDL2/SDL.h>
#include <SDL2/SDL_opengl.h>
#include <boost/json/serialize.hpp>
#include <iterator>

#ifndef BOOST_JSON_STACK_BUFFER_SIZE 
    # define BOOST_JSON_STACK_BUFFER_SIZE 1024
#endif

#include <fstream>

#include "cosim/PegasusCoSim.hpp"
#include "cosim/Event.hpp"

#include <boost/json.hpp>
#include <boost/json/stream_parser.hpp>

namespace json = boost::json;

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


int num_init = 0;

struct PipelineObject
{
    uint64_t    euid        = 0;
    uint64_t    pc          = 0;
    std::string dasm;
    uint32_t    opcode      = 0;
    int         step_num    = 0;

    bool        has_reg_reads  = false;
    bool        has_reg_writes = false;
    bool        has_mem_reads  = false;
    bool        has_mem_writes = false;
    bool        is_branch      = false;
    bool        faulted        = false;
    bool        is_last        = false;

    std::vector<std::pair<std::string, uint64_t>> reg_write_summary;
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

    std::vector<std::string> csr_names;
    std::vector<uint64_t> csr_vals;
    std::vector<uint64_t> csr_vals_prev;
     std::vector<bool> csr_changed;
    std::vector<bool> csr_exists;
    

    std::array<uint64_t, 32> reg_vals = {};
    std::array<uint64_t, 32> reg_vals_prev = {};
    std::array<bool, 32> reg_changed = {};

    std::vector<PipelineObject> pipeline;
    
    struct LogEntry{
        uint64_t euid;
        uint64_t pc;
        std::string dasm;
        uint32_t opcode;
        bool committed;
        bool has_reg_reads;
        bool has_reg_writes;
        bool has_mem_reads;
        bool has_mem_writes;
        bool is_branch;
        bool faulted;
        std::vector<std::pair<std::string,uint64_t>> reg_write_summary;
    };

    std::vector<LogEntry> logs;

    int total_step = 0;
    int total_commited = 0;
    bool error_occured = false;
    bool finish_called = false;
    std::string status_msg;
};

static std::vector<std::string> parse_csr_json(char const* filename)
{ 
    std::vector<std::string> names;
    std::ifstream f(filename);
    if(!f.is_open()){
        std::cerr << "Error opening the CSR json: " << filename << std::endl;
        return names;
    }

    std::string primeString((std::istreambuf_iterator<char>(f)), 
                                 std::istreambuf_iterator<char>());
    
    try {
        auto parsed_data = json::parse(primeString);

        if(parsed_data.is_array()){
            for(auto const& val : parsed_data.as_array()){
                const auto& obj = val.as_object();
                std::string name = json::value_to<std::string>(obj.at("name"));
                if(!name.empty())
                    names.push_back(name);
            }
        }
    } catch(const std::exception& e) {
        std::cerr << "JSON parse error: " << e.what() << std::endl;
    }
    
   return names;
}


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

    for(int i = 0 ; i < int(s.csr_names.size()); i++){
        if(!s.csr_exists[i]) continue;

        std::vector<uint8_t> buf(8,0);
        try{
            s.cosim->peekRegister(0,0, s.csr_names[i], buf);
        } catch(...){
            s.csr_exists[i] = false;
            continue;
        }

        uint64_t val = 0;
        for(int b = 0; b < 8 && b < (int)buf.size(); ++b){
            val |= (uint64_t)buf[b] << (8*b);
        }
        s.csr_changed[i] = (val != s.csr_vals[i]);
        s.csr_vals_prev[i] = s.csr_vals[i];
        s.csr_vals[i] = val;
    }
}


static void initialize(SimState& s)
{
    if (s.cosim != nullptr) return;
    if (s.running) return;
    s.running = true;
    s.status_msg = "Launching please wait..initializing pegasus...";
    s.error_occured = false;
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
        if(s.csr_names.empty()){
            s.csr_names = parse_csr_json("arch/default/rv64/gen/reg_csr_hart.json");
            std::cout << "Loaded Csr registers" << std::endl;
        }
        int n = (int)s.csr_names.size();
        s.csr_vals.assign(n,0);
        s.csr_vals_prev.assign(n,0);
        s.csr_changed.assign(n,false);
        s.csr_exists.assign(n,true);
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

        // evt has euid - the euid does not change it always remains the same - but the status of the event assosiated with the eventaccessor will change hence we can do the below thing.
        const pegasus::cosim::Event* e = evt.get();

        // build log entry with real event data from Pegasus
        SimState::LogEntry entry;
        entry.euid           = evt.getEuid(); // NOTE: USE DOT HERE - NO ARROW
        entry.pc             = e->getPc();
        entry.dasm           = e->getDisassemblyStr();
        entry.opcode         = e->getOpcode();
        entry.committed      = false;
        entry.has_reg_reads  = !e->getRegisterReads().empty();
        entry.has_reg_writes = !e->getRegisterWrites().empty();
        entry.has_mem_reads  = !e->getMemoryReads().empty();
        entry.has_mem_writes = !e->getMemoryWrites().empty();
        entry.is_branch      = e->isChangeOfFlowEvent();
        entry.faulted        = (e->getExceptionType() != pegasus::ExcpType::INVALID);

        for(const auto& rw : e->getRegisterWrites()){
            uint64_t val = 0;
            for(int b = 0; b < 8 && b < (int)rw.value.size(); ++b)
                val |= (uint64_t)rw.value[b] << (8*b);
            entry.reg_write_summary.push_back({rw.reg_id.reg_name, val});
        }

        s.logs.push_back(entry);
        
        s.cosim->commit(evt);
        s.logs.back().committed = true;
        s.total_commited += 1;
        s.total_step += 1;

        refreshRegisters(s);
        s.finished = s.cosim->isSimulationFinished(0, 0);
        if(s.finished){
            s.status_msg = "Finished Execution after " + std::to_string(s.total_step) + " steps";
            if(!s.finish_called){
                try{
                    s.cosim->finish();
                } catch (...){
                }
                s.finish_called = true; 
            }
        } else {
            s.status_msg = "Currently at PC : " + hex64(e->getPc()) + " " + e->getDisassemblyStr();
        }
        s.error_occured = false;
    } catch(std::exception& e){
        s.status_msg = std::string("ERROR: ") + e.what();
        s.step_automatically = false; // Incase the entire thing is running automatically and an error occurs - we dont want the simulator to keep on trying to run itself - we just want it to pause
        s.error_occured = true;
    }
}

static void drawEventLog(SimState& s, float x, float y, float W, float H)
{
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove
                           | ImGuiWindowFlags_NoCollapse
                           | ImGuiWindowFlags_NoBringToFrontOnFocus;

    ImGui::SetNextWindowPos({x, y});
    ImGui::SetNextWindowSize({W, H});
    if (ImGui::Begin("Instruction Log", nullptr, flags))
    {
        if (ImGui::BeginTable("evtlog", 9,
            ImGuiTableFlags_Borders       |
            ImGuiTableFlags_RowBg         |
            ImGuiTableFlags_ScrollY       |
            ImGuiTableFlags_SizingFixedFit,
            ImVec2(0, H - 40)))
        {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("EUID",   ImGuiTableColumnFlags_WidthFixed,   55);
            ImGui::TableSetupColumn("PC",     ImGuiTableColumnFlags_WidthFixed,  145);
            ImGui::TableSetupColumn("Opcode", ImGuiTableColumnFlags_WidthFixed,   85);
            ImGui::TableSetupColumn("rRegs",  ImGuiTableColumnFlags_WidthFixed,   45); // register reads
            ImGui::TableSetupColumn("wRegs",  ImGuiTableColumnFlags_WidthFixed,   45); // register writes
            ImGui::TableSetupColumn("Mem",    ImGuiTableColumnFlags_WidthFixed,   40); // memory access
            ImGui::TableSetupColumn("Br",     ImGuiTableColumnFlags_WidthFixed,   30); // branch/COF
            ImGui::TableSetupColumn("Flt",    ImGuiTableColumnFlags_WidthFixed,   30); // fault
            ImGui::TableSetupColumn("Disasm", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();

            for (auto it = s.logs.rbegin(); it != s.logs.rend(); ++it)
            {
                const auto& e = *it;

                // row color: fault = red, branch = orange, normal = white
                ImVec4 row_col = {0.9f, 0.9f, 0.9f, 1.0f};
                if(e.faulted)        row_col = {1.0f, 0.4f, 0.4f, 1.0f}; // red
                else if(e.is_branch) row_col = {1.0f, 0.85f, 0.4f, 1.0f}; // orange

                ImGui::TableNextRow();

                ImGui::TableSetColumnIndex(0);
                ImGui::TextColored(row_col, "%llu", (unsigned long long)e.euid);

                ImGui::TableSetColumnIndex(1);
                ImGui::TextColored(row_col, "%s", hex64(e.pc).c_str());

                ImGui::TableSetColumnIndex(2);
                ImGui::TextColored(row_col, "%s", hex32(e.opcode).c_str());

                // rRegs - green R if instruction read registers
                ImGui::TableSetColumnIndex(3);
                if(e.has_reg_reads)
                    ImGui::TextColored({0.4f, 1.0f, 0.4f, 1.0f}, "  R");
                else
                    ImGui::TextDisabled("  -");

                // wRegs - yellow W if instruction wrote registers
                // hover shows which registers changed and to what value
                ImGui::TableSetColumnIndex(4);
                if(e.has_reg_writes){
                    ImGui::TextColored({1.0f, 1.0f, 0.2f, 1.0f}, "  W");
                    if(ImGui::IsItemHovered() && !e.reg_write_summary.empty()){
                        ImGui::BeginTooltip();
                        for(const auto& rw : e.reg_write_summary){
                            ImGui::Text("%s = %s",
                                rw.first.c_str(),
                                hex64(rw.second).c_str());
                        }
                        ImGui::EndTooltip();
                    }
                } else {
                    ImGui::TextDisabled("  -");
                }

                // Mem - R, W, or RW
                ImGui::TableSetColumnIndex(5);
                if(e.has_mem_reads && e.has_mem_writes)
                    ImGui::TextColored({0.6f, 0.8f, 1.0f, 1.0f}, "RW");
                else if(e.has_mem_reads)
                    ImGui::TextColored({0.6f, 0.8f, 1.0f, 1.0f}, " R");
                else if(e.has_mem_writes)
                    ImGui::TextColored({0.6f, 0.8f, 1.0f, 1.0f}, " W");
                else
                    ImGui::TextDisabled("  -");

                // Branch/change of flow
                ImGui::TableSetColumnIndex(6);
                if(e.is_branch)
                    ImGui::TextColored({1.0f, 0.85f, 0.4f, 1.0f}, " B");
                else
                    ImGui::TextDisabled("  -");

                // Fault/exception
                ImGui::TableSetColumnIndex(7);
                if(e.faulted)
                    ImGui::TextColored({1.0f, 0.3f, 0.3f, 1.0f}, " !");
                else
                    ImGui::TextDisabled("  -");

                ImGui::TableSetColumnIndex(8);
                ImGui::TextColored(row_col, "%s", e.dasm.c_str());
            }
            ImGui::EndTable();
        }
    }
    ImGui::End();
}

// CSR register panel - changed CSRs float to the top every step
// non-existent CSRs (ones that threw on peekRegister) are silently skipped
static void drawCsrFile(SimState& s, float x, float y, float W, float H)
{
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove
                           | ImGuiWindowFlags_NoCollapse
                           | ImGuiWindowFlags_NoBringToFrontOnFocus;

    ImGui::SetNextWindowPos({x, y});
    ImGui::SetNextWindowSize({W, H});
    if (ImGui::Begin("CSR File", nullptr, flags))
    {
        if (ImGui::BeginTable("csrfile", 3,
            ImGuiTableFlags_Borders     |
            ImGuiTableFlags_RowBg       |
            ImGuiTableFlags_ScrollY     |
            ImGuiTableFlags_SizingFixedFit,
            ImVec2(0, H - 40)))
        {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("CSR",   ImGuiTableColumnFlags_WidthFixed,  120);
            ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Value (int)", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableHeadersRow();

            // build draw order every frame: changed first, then unchanged, skip non-existent
            // this is cheap - vector of ints, done every frame
            std::vector<int> order;
            order.reserve(s.csr_names.size());
            for(int i = 0; i < (int)s.csr_names.size(); i++)
                if(s.csr_exists[i] && s.csr_changed[i])
                    order.push_back(i);   // changed ones come first
            for(int i = 0; i < (int)s.csr_names.size(); i++)
                if(s.csr_exists[i] && !s.csr_changed[i])
                    order.push_back(i);   // unchanged ones after
            // non-existent ones never pushed - just absent from the table

            for(int idx : order)
            {
                // yellow if changed this step, white if unchanged
                ImVec4 col = s.csr_changed[idx]
                    ? ImVec4{1.0f, 1.0f, 0.2f, 1.0f}
                    : ImVec4{0.9f, 0.9f, 0.9f, 1.0f};

                ImGui::TableNextRow();

                ImGui::TableSetColumnIndex(0);
                ImGui::TextColored(col, "%s", s.csr_names[idx].c_str());

                ImGui::TableSetColumnIndex(1);
                ImGui::TextColored(col, "%s", hex64(s.csr_vals[idx]).c_str());
                // hover on value shows what it was before the change
                if(s.csr_changed[idx] && ImGui::IsItemHovered()){
                    ImGui::BeginTooltip();
                    ImGui::Text("prev: %s", hex64(s.csr_vals_prev[idx]).c_str());
                    ImGui::EndTooltip();
                }

                ImGui::TableSetColumnIndex(2);
                ImGui::TextColored(col, "%d", (int)(s.csr_vals[idx]));
            }
            ImGui::EndTable();
        }
    }
    ImGui::End();
}

static void resetSim(SimState& s)
{
    if(s.cosim){
        if(!s.finish_called){
            try{
            s.cosim->finish();
        } catch(...){}
        } 
        delete s.cosim;
        s.cosim = nullptr;
    }
    s.running = false;
    s.finished = false;
    s.finish_called = false;
    s.step_automatically = false;
    s.total_step = 0;
    s.total_commited = 0;
    s.logs.clear();
    s.pipeline.clear();
    s.reg_vals = {};
    s.reg_vals_prev = {};
    s.reg_changed = {};
    {
        int n = (int)s.csr_names.size();
        s.csr_vals.assign(n, 0);
        s.csr_vals_prev.assign(n, 0);
        s.csr_changed.assign(n, false);
        s.csr_exists.assign(n, true);
    }
    s.status_msg = "Reset.";
    s.error_occured = false;
}

// The above tool bar drawing
// currently I still need to add the method of taking a input in the header of the number of steps to walk and then walking the steps
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

        const float REG_W     = 300.0f;
        const float CONTENT_H = H - TOOLBAR_H;
        const float INT_REG_H = CONTENT_H * 0.35f;
        const float CSR_H     = CONTENT_H - INT_REG_H;

        drawToolbar     (state, W, TOOLBAR_H);
        drawRegisterFile(state, 0,       TOOLBAR_H,                REG_W,     INT_REG_H);
        drawCsrFile     (state, 0,       TOOLBAR_H + INT_REG_H,    REG_W,     CSR_H);
        drawEventLog    (state, REG_W,   TOOLBAR_H,                W - REG_W, CONTENT_H);
        
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