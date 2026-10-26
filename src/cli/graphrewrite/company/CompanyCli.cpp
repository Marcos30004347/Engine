#include "Company.hpp"

#include <charconv>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <functional>
#include <chrono>
#include <thread>
#include <algorithm>
#include <cctype>
#include <sstream>
#include <deque>
#include <stdexcept>
#ifdef _WIN32
#include <conio.h>
#include <io.h>
#else
#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace company {
namespace {
struct Quit {};

bool terminal() {
#ifdef _WIN32
  return _isatty(_fileno(stdin)) && _isatty(_fileno(stdout));
#else
  const char* term = std::getenv("TERM");
  return isatty(STDIN_FILENO) && isatty(STDOUT_FILENO) && (!term || std::string(term) != "dumb");
#endif
}
#ifndef _WIN32
// Ctrl-C is returned as a key so RAII restores the terminal before leaving.
// No alternate screen or hidden cursor to restore.
// Raw mode is scoped to a menu or animation, and restored on every exit path.
class KeyMode {
  termios saved{};
  bool active = false;
public:
  KeyMode() {
    if (tcgetattr(STDIN_FILENO, &saved) != 0) throw std::runtime_error("Cannot read terminal settings");
    auto raw = saved;
    raw.c_lflag &= static_cast<tcflag_t>(~(ICANON | ECHO));
    raw.c_lflag &= static_cast<tcflag_t>(~ISIG); // Ctrl-C is handled after RAII restoration.
    raw.c_cc[VMIN] = 1; raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) throw std::runtime_error("Cannot set terminal input mode");
    active = true;
  }
  ~KeyMode() { if (active) tcsetattr(STDIN_FILENO, TCSANOW, &saved); }
};
int readByte(int timeout = -1) {
  pollfd p{STDIN_FILENO, POLLIN, 0};
  if (poll(&p, 1, timeout) <= 0) return -1;
  unsigned char c;
  return read(STDIN_FILENO, &c, 1) == 1 ? c : -1;
}
#endif
int key() {
#ifdef _WIN32
  int c = _getch();
  if (c == 224 || c == 0) { c = _getch(); return c == 72 ? 'k' : c == 80 ? 'j' : 0; }
  return c;
#else
  int c = readByte();
  if (c == 27) {
    int next = readByte(80);
    if (next == '[' || next == 'O') {
      std::string sequence;
      for(int i=0;i<24;++i) { int part=readByte(80); if(part<0) break; sequence+=static_cast<char>(part); if(part>=64 && part<=126) break; }
      if(sequence=="A") return 'k'; if(sequence=="B") return 'j';
      // CSI-u Super+A (Command on macOS), when forwarded by the terminal.
      if(sequence=="97;9u" || sequence=="97;5u") return 1;
      return 0;
    }
    return 0;
  }
  return c;
#endif
}
uint32_t number(const std::string& value) {
  uint32_t result = 0;
  auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
  if (value.empty() || parsed.ec != std::errc() || parsed.ptr != value.data() + value.size())
    throw std::invalid_argument("Expected an integer from 0 to 4294967295");
  return result;
}
class UI {
  bool arrows, animate;
  bool unread = false;
  bool fast = false;
  std::deque<int> pending;
  std::ofstream transcript;
  std::function<std::string()> header;
  std::function<std::string(std::string)> prose = [](std::string t) { return t; };
  void record(const std::string& text) {
    if (transcript.is_open()) {
      transcript << '\n' << text << "\n"; transcript.flush();
      if (!transcript) throw std::runtime_error("Cannot write transcript");
    }
  }
public:
  static constexpr size_t manage = std::numeric_limits<size_t>::max();
  static constexpr size_t inspect = manage - 1;
  static constexpr size_t nodeBase = 10000;
  static constexpr const char* nodeLabels = "AESDWPIRO";
  explicit UI(bool plain, bool noAnimation, const std::string& output)
    : arrows(!plain && terminal()), animate(arrows && !noAnimation) {
    if (!output.empty()) {
      transcript.open(output);
      if (!transcript) throw std::runtime_error("Cannot write transcript: " + output);
    }
  }
  ~UI() { if(arrows) std::cout << "\033[0m" << std::flush; }
  void setProse(std::function<std::string(std::string)> value) { prose = std::move(value); }
  void setHeader(std::function<std::string()> value) { header = std::move(value); }
  void say(const std::string& text) { const auto rendered = prose(text); std::cout << '\n' << rendered << "\n"; record(rendered); }
  void type(const std::string& text) {
    if (!animate) { std::cout << text; return; }
#ifndef _WIN32
    KeyMode mode;
#endif
    const int delay=std::min(12,2400/std::max(1,static_cast<int>(text.size())));
    for (char ch:text) {
      if(!fast) {
#ifdef _WIN32
        const int input=_kbhit()?key():-1;
#else
        pollfd available{STDIN_FILENO,POLLIN,0};
        const int input=poll(&available,1,0)>0?key():-1;
#endif
        if(input==3 || input==4 || input=='q') throw Quit{};
        if(input==' ' || input=='\r' || input=='\n') fast=true;
        else if(input>=0) { pending.push_back(input); fast=true; }
      }
      std::cout << ch << std::flush;
      if(!fast) std::this_thread::sleep_for(std::chrono::milliseconds(
        ch=='.'||ch=='!'||ch=='?'?140:ch=='\n'?90:delay));
    }
  }
  void narrate(const std::string& source) {
    const auto text=prose(source); record(text);
    std::cout << '\n'; type(text); std::cout << "\n" << std::flush;
    unread=true;
  }
  void scene(const std::string& text) {
    // A result must remain readable before another scene replaces the page.
    if(arrows && unread) menu("When you are ready",{{"Continue the story"}},false);
    fast=false;
    if (arrows) std::cout << "\033[2J\033[H";
    if (header) {
      if(arrows) std::cout << "\033[1;36m";
      say(header());
      if(arrows) std::cout << "\033[0m";
    }
    narrate(text);
  }
  size_t menu(const std::string& title, const std::vector<Option>& options, bool allowDesk = true, const std::string& labelKeys = "", const std::vector<std::string>& aliases = {}) {
    unread=false;
    if(arrows) std::cout << "\033[1;33m";
    narrate(title); unread=false;
    if(arrows) std::cout << "\033[0m";
    if(!arrows) for(size_t i=0;i<options.size();++i) std::cout << "  " << i+1 << ". " << prose(options[i].label) << '\n';
    size_t selected = 0;
    std::string typedLabel;
    if (arrows) {
#ifndef _WIN32
      KeyMode mode; // Install before the prompt is visible; even immediate Ctrl-C is a key.
#endif
      std::cout << "Up/Down or j/k, Enter to choose; q to leave. "
                << (allowDesk ? "m: desk; Ctrl+A / a: architecture; uppercase node label: inspect. " : "") << "Space/Enter skips story animation.\n";
      int columns=100;
#ifndef _WIN32
      winsize size{}; if(ioctl(STDOUT_FILENO,TIOCGWINSZ,&size)==0 && size.ws_col>20) columns=size.ws_col;
#endif
      bool first=true;
      size_t renderedRows=0;
      while (true) {
        if(!first) std::cout << "\r\033[" << renderedRows << "A\033[J";
        renderedRows=options.size();
        for(size_t i=0;i<options.size();++i) {
          std::cout << (i==selected?"\033[1;32m  > ":"\033[0;37m    ");
          auto label=prose(options[i].label);
          if(label.size()>static_cast<size_t>(columns-6)) {
            size_t cut=columns-10;
            while(cut>0 && (static_cast<unsigned char>(label[cut])&0xc0)==0x80) --cut;
            label=label.substr(0,cut)+"...";
          }
          if(first) type(label); else std::cout << label;
          std::cout << "\033[0m\n";
        }
        first=false;
        auto detail=prose(options[selected].label);
        std::cout << "\033[36m";
        while(!detail.empty()) {
          size_t cut=std::min(detail.size(),static_cast<size_t>(columns-2));
          if(cut<detail.size()) {
            while(cut>0 && (static_cast<unsigned char>(detail[cut])&0xc0)==0x80) --cut;
            const auto space=detail.rfind(' ',cut);
            if(space!=std::string::npos && space>0) cut=space;
          }
          std::cout << detail.substr(0,cut) << '\n'; ++renderedRows;
          detail.erase(0,cut);
          if(!detail.empty() && detail.front()==' ') detail.erase(0,1);
        }
        std::cout << "\033[0m";
        std::cout << "\033[2mSelected option " << selected+1 << "\033[0m" << std::flush;
        int c;
        if(pending.empty()) c=key(); else { c=pending.front(); pending.pop_front(); }
        if (c == 'q' || c == 'Q' || c == 3 || c == 4 || c < 0) throw Quit{};
        if(!aliases.empty() && (std::isalpha(c) || !typedLabel.empty())) {
          if(std::isalnum(c)) typedLabel+=static_cast<char>(std::toupper(c));
          else typedLabel.clear();
          const auto match=std::find(aliases.begin(),aliases.end(),typedLabel);
          bool prefix=false,longer=false;
          for(const auto& alias:aliases) if(!typedLabel.empty() && alias.rfind(typedLabel,0)==0) { prefix=true; longer|=alias.size()>typedLabel.size(); }
          if(!typedLabel.empty() && match!=aliases.end()) {
            selected=match-aliases.begin();
            if(!longer) break;
          }
          if(!prefix) typedLabel.clear();
          if(!typedLabel.empty()) continue;
        }
        if (allowDesk && c == 'm') return manage;
        if (allowDesk && (c == 'a' || c == 1)) return inspect;
        if (allowDesk && std::string(nodeLabels).find(static_cast<char>(c)) != std::string::npos)
          return nodeBase + std::string(nodeLabels).find(static_cast<char>(c));
        const auto shortcut = labelKeys.find(static_cast<char>(std::toupper(c)));
        if (!labelKeys.empty() && shortcut != std::string::npos) { selected = shortcut; std::cout << '\n'; break; }
        if (c == 'k') selected = (selected + options.size() - 1) % options.size();
        if (c == 'j') selected = (selected + 1) % options.size();
        if (c >= '1' && c <= '9' && static_cast<size_t>(c - '1') < options.size()) selected = c - '1';
        if (c == '\r' || c == '\n') { std::cout << '\n'; break; }
      }
    } else {
      while (true) {
        std::cout << "Choice [1-" << options.size() << (allowDesk ? ", m: desk" : "") << ", Enter: first option, q: leave]: " << std::flush;
        std::string line;
        if (!std::getline(std::cin, line) || line == "q" || line == "Q") throw Quit{};
        if(line.empty()) break;
        std::string label=line;
        for(auto& c:label) c=static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        const auto alias=std::find(aliases.begin(),aliases.end(),label);
        if(!label.empty() && alias!=aliases.end()) { selected=alias-aliases.begin(); break; }
        if (allowDesk && line == "m") return manage;
        if (allowDesk && line == "a") return inspect;
        if (allowDesk && line.size() == 1 && std::string(nodeLabels).find(line[0]) != std::string::npos)
          return nodeBase + std::string(nodeLabels).find(line[0]);
        if (line.size() == 1 && !labelKeys.empty()) {
          const auto shortcut = labelKeys.find(static_cast<char>(std::toupper(static_cast<unsigned char>(line[0]))));
          if (shortcut != std::string::npos) { selected = shortcut; break; }
        }
        try {
          auto n = number(line);
          if (n >= 1 && n <= options.size()) { selected = n - 1; break; }
        } catch (const std::invalid_argument&) {}
        std::cout << "Please choose one of the listed numbers.\n";
      }
    }
    if(arrows) std::cout << "\033[1;32m";
    say("You chose: " + options[selected].label);
    if(arrows) std::cout << "\033[0m";
    return selected;
  }
};}
int runCli(int argc, char** argv) {
  try {
    bool plain = false, demo = false, noAnimation = false, suppliedSeed = false, suppliedPath = false, suppliedDifficulty = false;
    uint32_t seed = 1;
    std::string output;
    Business business = Business::Video;
    Difficulty difficulty = Difficulty::Normal;
    for (int i = 1; i < argc; ++i) {
      std::string arg = argv[i];
      if (arg == "--help" || arg == "-h") {
        std::cout << "company-story [--seed N] [--path video|game] [--difficulty easy|normal|hard] [--plain] [--no-animation] [--demo] [--out FILE]\n"
          "company-story --list-paths\n"
          "An interactive, seeded, 52-week company story (roughly 45-75 minutes; no forced timer).\n"
          "Arrow keys / j,k and Enter select; numbers work with --plain or redirected input.\n"
          "--demo plays a repeatable example without input. --out saves the chronicle.\n"
          "Also available as graphrewrite-story --company [options].\n";
        return 0;
      } else if (arg == "--list-paths") {
        std::cout << "video   Video / film streaming (playable)\n"
                     "game    Fantasy game studio (playable)\n";
        return 0;
      } else if (arg == "--plain") plain = true;
      else if (arg == "--demo") demo = true;
      else if (arg == "--no-animation") noAnimation = true;
      else if ((arg == "--seed" || arg == "--out" || arg == "--path" || arg == "--difficulty") && i + 1 < argc) {
        std::string value = argv[++i];
        if (arg == "--seed") { seed = number(value); suppliedSeed = true; }
        else if (arg == "--out") output = value;
        else if (arg == "--difficulty") {
          if(value!="easy" && value!="normal" && value!="hard") throw std::invalid_argument("Choose --difficulty easy, normal or hard");
          suppliedDifficulty=true;
          difficulty=value=="easy"?Difficulty::Easy:value=="hard"?Difficulty::Hard:Difficulty::Normal;
        }
        else {
          if (value != "video" && value != "game" && value != "fantasy") throw std::invalid_argument("Choose --path video or --path game");
          business = value == "video" ? Business::Video : Business::Game;
          suppliedPath = true;
        }
      } else throw std::invalid_argument("Unknown or incomplete option: " + arg);
    }
    UI ui(plain || demo, noAnimation, output);
    ui.say("SMALL TEAM, BIG CLOUD\nA startup chronicle about architecture, people, and expensive assumptions.");
    if (!suppliedSeed && !demo) {
      while (true) {
        std::cout << "Seed [0-4294967295, Enter for 1, q to leave]: " << std::flush;
        std::string line;
        if (!std::getline(std::cin, line) || line == "q") throw Quit{};
        try { seed = line.empty() ? 1 : number(line); break; }
        catch (const std::invalid_argument& e) { std::cout << e.what() << '\n'; }
      }
    }
    if (!suppliedPath && !demo) {
      const auto path = ui.menu("Choose a company path", {{"Video / film streaming"}, {"Fantasy game studio"}}, false);
      business = path == 0 ? Business::Video : Business::Game;
    }
    if(!demo && !suppliedPath && !suppliedDifficulty) {
      const auto level=ui.menu("Development window",{{"Normal: 18 weeks"},{"Easy: 24 weeks"},{"Hard: 12 weeks"}},false);
      difficulty=level==1?Difficulty::Easy:level==2?Difficulty::Hard:Difficulty::Normal;
    }
    ui.say("Seed " + std::to_string(seed) + (business == Business::Video ? " | Video / film streaming" : " | Fantasy game studio") + " | 52 weeks");
    Game game(seed, business, difficulty);
    ui.setProse([&](std::string text) { return game.localize(std::move(text)); });
    ui.setHeader([&] { return game.dashboard(); });
    ui.scene(game.introduction());
    std::function<void()> desk;
    std::function<void(std::string)> architectureDesk;
    std::function<void()> designDesk;
    auto decide = [&](const Decision& original, size_t demoChoice = 0, bool allowDesk = true, bool workerQuestion = false, bool teamProposal = false) {
      bool alternatives=false;
      while (true) {
        // A colleague can leave while this prompt is suspended at the company desk.
        if (workerQuestion && original.speaker != "Founder" && std::none_of(game.state().team.begin(), game.state().team.end(),
            [&](const Person& p) { return p.name == original.speaker; })) return;
        auto d = game.decision(original.speaker=="Architecture review"?"architecture_review":original.id);
        if (workerQuestion) d.speaker = original.speaker;
        for (const auto& node : game.architectureNodes()) if (node.decision == d.id && game.state().phase != Phase::Development)
          d.text += " Live configuration changes cost $900 per changed node; IAM setup is $600.";
        if (d.id == "raise" && game.state().raiseName.empty()) return;
        size_t selection=demoChoice;
        if(!demo && teamProposal && !alternatives) {
          const auto proposal=game.proposal(d);
          selection=ui.menu(proposal.speaker+": "+proposal.text+"\n\n"+d.text+"\n\nProposed decision: "+d.options[proposal.option].label,
            {{"Continue with this decision"},{"Choose another option"}},allowDesk);
          if(selection==1) { alternatives=true; continue; }
          if(selection==0) selection=proposal.option;
        } else if(!demo) selection=ui.menu(d.speaker+": "+d.text,d.options,allowDesk);
        if (selection == UI::manage || selection == UI::inspect || (selection >= UI::nodeBase && selection < UI::nodeBase + 9)) {
          if (selection == UI::manage) desk();
          else architectureDesk(selection == UI::inspect ? "" : std::string(1, UI::nodeLabels[selection - UI::nodeBase]));
          if (game.bankrupt()) return;
          continue;
        }
        if (demo) ui.say(d.speaker + ": " + d.text + "\nYou chose: " + d.options[selection].label);
        const auto result = game.choose(d, selection);
        ui.scene(result);
        return;
      }
    };
    designDesk = [&] {
      while(true) {
        ui.scene(game.designGraph());
        const auto action=ui.menu("Rewrite the system graph",{{"Move an API to a server / extract it into Lambda"},{"Move a dataset or reconcile a migration"},
          {"Configure / retire a compute node"},{"Configure / pool / retire a database"},{"Add a server placeholder"},{"Add a database placeholder"},
          {"Rehearse API-to-backend paths ($200)"},{"Back to the architecture map"}},false);
        if(action==7) return;
        if(action==6) { ui.scene(game.exercise("G")); ui.menu("Rehearsal complete",{{"Continue"}},false); continue; }
        if(action==4 || action==5) {
          const auto previous=game.state().design.compute.size()+game.state().design.databases.size();
          decide(game.decision(action==4?"design:add-compute":"design:add-database"),0,false);
          const auto& graph=game.state().design;
          if(graph.compute.size()+graph.databases.size()>previous)
            decide(game.decision(action==4?"design:compute:"+graph.compute.back().id:"design:database:"+graph.databases.back().id),0,false);
          ui.menu("Node update complete",{{"Continue"}},false);
          continue;
        }
        std::vector<Option> nodes; std::vector<std::string> labels;
        const auto& graph=game.state().design;
        if(action==0) for(const auto& api:graph.apis) { nodes.push_back({api.id+" | "+api.name+" on "+api.compute}); labels.push_back(api.id); }
        if(action==1) for(const auto& data:graph.data) { nodes.push_back({data.id+" | "+data.name+" in "+data.database}); labels.push_back(data.id); }
        if(action==2) for(const auto& c:graph.compute) { nodes.push_back({c.id+" | "+c.name}); labels.push_back(c.id); }
        if(action==3) for(const auto& db:graph.databases) { nodes.push_back({db.id+" | "+db.name}); labels.push_back(db.id); }
        nodes.push_back({"Back"}); labels.push_back("X");
        const auto selected=ui.menu("Select a node by number or its label (for example U3, B2, C2, D2). X returns.",nodes,false,"",labels);
        if(selected+1==nodes.size()) continue;
        const auto id=labels[selected];
        if(action==0) { ui.scene(game.designGraph(id)); decide(game.decision("design:route:"+id),0,false); }
        if(action==1) {
          const auto task=ui.menu("Dataset operation",{{"Migrate this dataset to another backend"},{"Reconcile records and validate access patterns"},{"Back"}},false);
          if(task<2) decide(game.decision((task==0?"design:data:":"design:verify:")+id),0,false);
        }
        if(action==2) {
          const auto task=ui.menu("Compute operation",{{"Choose an AWS service"},{"Retire unused compute"},{"Back"}},false);
          if(task<2) decide(game.decision((task==0?"design:compute:":"design:retire:")+id),0,false);
        }
        if(action==3) {
          const auto task=ui.menu("Database operation",{{"Choose / migrate database engine"},{"Set connection pooling"},{"Retire unused database"},{"Back"}},false);
          const char* operations[]={"design:database:","design:pool:","design:retire:"};
          if(task<3) decide(game.decision(std::string(operations[task])+id),0,false);
        }
        // Do not erase the migration result before it can be read.
        ui.menu("Graph update complete",{{"Continue designing"}},false);
      }
    };
    architectureDesk = [&](std::string selectedLabel) {
      std::string system;
      if(!selectedLabel.empty()) system = selectedLabel=="S" || selectedLabel=="E" ? "4" : selectedLabel=="D" ? "2" : selectedLabel=="W" ? "5" : selectedLabel=="A" ? "1" : "6";
      while (true) {
        if (selectedLabel.empty()) {
          if(system.empty()) {
            ui.scene(game.systemGraph());
            const auto chosen=ui.menu("Choose a high-level system",{{"Accounts"},{"Catalogue"},{"Payments"},{"Playback / sessions"},{"Ingestion / workers"},{"Operations"},{"Return to story"},{"Edit API ownership, datasets and service nodes"},{"Architecture report: dependencies, costs and changes"}},false);
            if(chosen==8) { ui.scene(game.architectureReport()); ui.menu("Architecture report",{{"Return to map"}},false); continue; }
            if(chosen==7) { designDesk(); continue; }
            if(chosen==6) { ui.scene("You return to the pending conversation."); return; }
            system=std::to_string(chosen+1);
          }
          ui.scene(game.systemGraph(system));
          auto labels=game.systemComponents(system);
          std::vector<Option> parts;
          for(const auto& label:labels) for(const auto& n:game.architectureNodes()) if(n.label==label) parts.push_back({"["+n.label+"] "+n.service});
          parts.push_back({"[X] Back to system map"});
          labels.push_back("X");
          const auto part=ui.menu("Select an AWS component",parts,false,"",labels);
          if(part+1==labels.size()) { system.clear(); continue; }
          selectedLabel=labels[part];
        }
        const auto id = game.nodeDecision(selectedLabel);
        const auto d = game.decision(id);
        auto options = d.options;
        options.push_back({"Run a $200 tabletop event against this node"});
        options.push_back({"Back to the architecture graph"});
        ui.scene(system.empty()?game.systemGraph():game.systemGraph(system));
        const auto selected = ui.menu("[" + selectedLabel + "] " + d.text +
          "\nLive service changes cost $900; IAM setup costs $600. Prototype changes have no migration fee.", options, false);
        if (selected < d.options.size()) ui.scene(game.choose(d, selected));
        else if (selected == d.options.size()) ui.scene(game.exercise(selectedLabel));
        else { selectedLabel.clear(); continue; }
        // Keep the result visible until the player chooses where to go next.
        const auto next = ui.menu("Architecture action complete", {{"Return to the story"}, {"Inspect another node"}, {"More options for this node"}}, false);
        if (next == 0) return;
        if (next == 1) selectedLabel.clear();
      }
    };
    desk = [&] {
      ui.scene("The company notebook is open. The story can wait while you look after the people and the money.");
      while (true) {
        size_t action = ui.menu("Company desk", {{"Return to the story"}, {"Hire a human or activate an AI model"},
          {"Lay off an employee / stop an agent"}, {"Costs, available money and runway"}, {"Ask for investment"},
          {"Revisit AWS architecture"}, {"Production supervision"}, {"Team workload and morale"}, {"Last monthly review and project board"}}, false);
        if (action == 0) { ui.scene("You close the notebook and return to the week's conversation."); break; }
        if (action == 1) { game.refreshCandidates(); decide(game.decision("hire"), 0, false); }
        if (action == 2) {
          for (const auto& p : game.state().team) ui.say(p.description());
          decide(game.decision("release"), 0, false);
        }
        if (action == 3) ui.scene(game.finances());
        if (action == 4) decide(game.decision("investment"), 0, false);
        if (action == 5) architectureDesk("");
        if (action == 6) decide(game.decision("review"), 0, false);
        if (action == 7) decide(game.decision("rest"), 0, false);
        if (action == 8) ui.scene(game.monthlyReview() + "\n" + game.taskBoard());
      }
    };
    decide(game.decision("hire"), 3);
    for (int week = 1; week <= Game::campaignWeeks && !game.bankrupt(); ++week) {
      const auto monday = game.beginWeek();
      ui.scene(monday);
      ui.say(game.taskBoard());
      const auto advice=game.hiringAdvice();
      if(!demo && !advice.empty()) {
        const auto hire=ui.menu(advice,{{"Keep the current team"},{"Meet fresh candidates"}},false);
        if(hire==1) { game.refreshCandidates(); decide(game.decision("hire"),0,false); }
      }
      std::vector<std::string> planned;
      while(!game.bankrupt()) {
        const auto people=game.planners();
        const auto next=std::find_if(people.begin(),people.end(),[&](const std::string& who){return std::find(planned.begin(),planned.end(),who)==planned.end();});
        if(next==people.end()) break;
        const auto person=*next; planned.push_back(person);
        decide(game.decision("plan:"+person));
      }
      ui.narrate(game.prepareWork());
      auto chapter = game.chapter();
      const bool architectural=chapter.id=="design" || chapter.id.rfind("design:",0)==0 ||
        chapter.id=="api" || chapter.id=="database" || chapter.id=="storage" || chapter.id=="encoding" ||
        chapter.id=="scaling" || chapter.id=="monitoring" || chapter.id=="identity" || chapter.id=="delivery" || chapter.id=="recovery";
      decide(chapter, chapter.id == "recovery" ? 2 : 0,true,false,architectural);
      if (!game.bankrupt()) {
        for (const auto& question : game.questions()) {
          decide(question, 0, true, true,true);
          if (game.bankrupt()) break;
        }
      }
      if (!game.bankrupt()) {
        ui.narrate(game.resolveWork());
        ui.narrate(game.event());
        if (!game.state().raiseName.empty()) decide(game.decision("raise"));
        if (game.state().incidentThisWeek && !game.bankrupt()) decide(game.decision("incident"));
      }
      // Settle an open week even if an immediate expense exhausted the available cash.
      ui.narrate(game.closeWeek());
      if (game.bankrupt()) break;
      if (!demo) {
        ui.scene("Friday desk / the week is settled.");
        while (true) {
          size_t next = ui.menu(week == Game::campaignWeeks ? "The last Friday" : "End of week: take a breath before Monday",
            {{week == Game::campaignWeeks ? "Read the closing chapter" : "Begin next week"}, {"Open the company desk"}});
          if (next == 0) break;
          if (next == 1 || next == UI::manage) desk();
          else architectureDesk(next == UI::inspect ? "" : std::string(1, UI::nodeLabels[next - UI::nodeBase]));
        }

      }
    }
    ui.say(game.ending());
    return 0;
  } catch (const Quit&) {
    std::cout << "\nThe screening is paused here. Start again with the same seed to replay; progress is not saved.\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "company-story: " << e.what() << '\n';
    return 1;
  }
}
}
