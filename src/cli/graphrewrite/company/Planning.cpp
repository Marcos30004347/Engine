#include "Company.hpp"
#include <algorithm>
#include <sstream>
#include <stdexcept>

namespace company {
Proposal Game::proposal(const Decision& d) const {
  if(d.options.empty()) throw std::invalid_argument("Cannot propose an empty decision");
  // Stateless seeded sampling: inspecting or reopening a proposal cannot reroll it.
  uint32_t sample=s.storySeed ^ (static_cast<uint32_t>(s.week)*2654435761u);
  for(unsigned char ch:d.id) sample=(sample^ch)*16777619u;
  auto person=worker("Founder");
  if(!s.team.empty()) person=s.team[sample%s.team.size()];
  for(const auto& p:s.team) if(p.name==d.speaker) person=p;
  bool contextual=d.id=="import_policy" || d.id=="context" || d.id=="retention";
  std::string action=d.id;
  if(d.id.rfind("task:",0)==0) {
    const auto task=static_cast<size_t>(std::stoul(d.id.substr(5)));
    contextual=s.tasks.at(task).contextual;
    action=implementationDecision(static_cast<int>(task));
    person=worker(d.speaker);
  }
  for(unsigned char ch:person.name) sample=(sample^ch)*16777619u;
  const int ability=contextual?person.context:person.technical;
  size_t preferred=action=="recovery"?2:action=="storage"?1:0;
  if(action.rfind("design:compute:",0)==0) {
    const auto id=action.substr(15);
    for(const auto& api:s.design.apis) if(api.compute==id)
      for(const auto& data:s.design.data) if(std::find(api.data.begin(),api.data.end(),data.id)!=api.data.end())
        for(const auto& db:s.design.databases) if(db.id==data.database && db.service>0 && !db.pooled) preferred=1;
  }
  preferred=std::min(preferred,d.options.size()-1);
  size_t option=preferred;
  if(sample%100>=static_cast<uint32_t>(ability) && d.options.size()>1)
    option=(preferred+1+(sample/100)%(d.options.size()-1))%d.options.size();
  auto text=rule("proposal",std::string("Company configured ")+(contextual?"proposalcontext":"proposaltechnical")+"\n").text;
  return {person.name+(person.human()?" (human)":" (AI agent)"),text,option};
}
Person Game::candidate(int index) const {
  if (index < 0 || index >= 7) throw std::invalid_argument("Unknown candidate");
  auto result = s.applicants[index]; result.monthlyCost = s.prices[index]; return result;
}
void Game::setupProject() {
  s.tasks = {
    {"Build the catalogue API and data model", "database", {}},
    {"Build account registration and authentication", "identity", {}},
    {"Build the subscription and payment API", "api", {1}},
    {s.business == Business::Game ? "Build matchmaking and the game client" : "Build the playback API and viewing app", "delivery", {0,1}},
    {s.business == Business::Game ? "Build the persistent session service" : "Build video ingestion and encoding", "encoding", {0}},
    {"Build deployment, monitoring and operations", "monitoring", {1}},
    {s.business == Business::Game ? "Build player onboarding and privacy controls" : "Build customer onboarding, consent and content entitlements", "import_policy", {1,2},0,true},
    {"Rehearse production recovery and failover", "drill", {5}},
    {"Review production reliability and permissions", "identity", {}},
    {"Talk to customers and improve the experience", "retention", {},0,true}
  };
}
std::vector<std::string> Game::planners() const {
  std::vector<std::string> people{"Founder"};
  for (const auto& p : s.team) people.push_back(p.name);
  return people;
}
Person Game::worker(const std::string& name) const {
  if (name == "Founder") return {"Founder", "learning on the job", 30, -1, 55, 85, 0};
  for (const auto& p : s.team) if (p.name == name) return p;
  throw std::invalid_argument("Worker is no longer available");
}
std::vector<int> Game::availableTasks() const {
  std::vector<int> available;
  for (int i = 0; i < static_cast<int>(s.tasks.size()); ++i) {
    const auto& t = s.tasks[i];
    if (i < 7 && t.progress >= 100) continue;
    if (i == 7 && s.testedRecovery) continue;
    if (i >= 7 && s.phase != Phase::Maintenance && s.phase != Phase::Launch) continue;
    if (std::any_of(t.dependencies.begin(),t.dependencies.end(),[&](int d) { return s.tasks[d].progress < 100; })) continue;
    if (std::any_of(s.assignments.begin(),s.assignments.end(),[&](const Assignment& a) { return a.task == i; })) continue;
    available.push_back(i);
  }
  return available;
}
std::string Game::taskBoard(bool monthly) const {
  std::ostringstream out;
  out << "PROJECT BOARD\n";
  for (int i = 0; i < static_cast<int>(s.tasks.size()); ++i) {
    const auto& t = s.tasks[i];
    out << "T" << i+1 << " " << t.title << " | ";
    if (i == 7 && s.testedRecovery) out << "current recovery plan rehearsed";
    else if (i >= 7) out << "ongoing service";
    else if (monthly) out << t.progress << "% delivered";
    else out << (t.progress >= 100 ? "delivered" : t.progress > 0 ? "in development" : "not started");
    if (!t.dependencies.empty()) {
      out << " | needs"; for (int dependency : t.dependencies) out << " T" << dependency+1;
    }
    for (const auto& a : s.assignments) if (a.task == i) out << " | this week: " << a.worker;
    out << '\n';
  }
  return out.str();
}
bool Game::readyToLaunch() const {
  return designReady() && std::all_of(s.tasks.begin(),s.tasks.begin()+7,[](const ProjectTask& t) { return t.progress >= 100; });
}
std::string Game::monthlyReview() const {
  return s.lastMonthlyReport.empty() ? "The first measured delivery review is at the end of week four. The board shows qualitative status until then." : s.lastMonthlyReport;
}
std::string Game::prepareWork() {
  if (!s.weekOpen || s.workPrepared) throw std::logic_error("Work must be prepared once after Monday planning");
  s.workPrepared = true;
  for (auto& a : s.assignments) {
    const auto people = planners();
    if (std::find(people.begin(),people.end(),a.worker) == people.end()) continue;
    auto& task = s.tasks[a.task];
    if (a.task >= 7) task.designMade = false;
    if (task.designMade) continue;
    const auto p = worker(a.worker);
    const int ability = task.contextual ? p.context : p.technical;
    // Founder asks the player; stronger workers usually make their own implementation decisions.
    a.needsAnswer = a.worker == "Founder" || roll(100) >= ability;
  }
  return s.assignments.empty() ? "Tuesday is quiet. With no work assigned, no part of the system moves forward." :
    "Tuesday. The planning notes become work: schema sketches, test accounts and half-finished pull requests. Questions arrive as the details become real.";
}
std::string Game::resolveWork() {
  if (!s.workPrepared || s.workResolved) throw std::logic_error("Resolve prepared work once");
  s.workResolved = true;
  std::ostringstream out;
  const auto people = planners();
  for (auto& a : s.assignments) {
    if (std::find(people.begin(),people.end(),a.worker) == people.end()) {
      out << "The owner of '" << s.tasks[a.task].title << "' has left. The unfinished work returns to the board.\n"; continue;
    }
    auto& task = s.tasks[a.task];
    if (a.needsAnswer) { out << a.worker << " is still waiting for a decision. That task does not ship this week.\n"; continue; }
    const auto p = worker(a.worker);
    const int ability = task.contextual ? p.context : p.technical;
    if (!task.designMade) {
      bool correct = roll(100) < ability;
      // Review mitigates agent implementation errors; it is not an infallible safety switch.
      if (!correct && !p.human() && s.reviewed) {
        int reviewer = 55;
        for (const auto& human : s.team) if (human.human()) reviewer = std::max(reviewer, human.technical);
        if (roll(100) < reviewer) correct = true;
      }
      int alternative = 1;
      if (task.decision == "database" || task.decision == "api") alternative = 2;
      // Applying the decision changes the real architecture. Its correctness is not announced.
      const auto implementation=decision(implementationDecision(a.task));
      const size_t selected=correct?0:static_cast<size_t>(alternative);
      choose(implementation,selected);
      out << a.worker << " records an implementation decision: " << implementation.options.at(selected).label
          << ". The change is now part of the system.\n";
      task.designMade = true;
    }
    if (a.task < 7) {
      const int effort = std::max(8, 30 + ability / 2 + roll(10) + (s.projectScope == 1 ? -15 : s.projectScope == 2 ? 12 : 0));
      const bool wasDone = task.progress >= 100;
      task.progress = std::min(100,task.progress + effort);
      out << a.worker << (task.progress >= 100 && !wasDone ? " hands over '" : " continues '" ) << task.title
          << (task.progress >= 100 ? "'. The team records it as delivered.\n" : "'. There is more to finish next week.\n");
    } else {
      if (a.task == 8) s.quality = std::min(100,s.quality+3);
      out << a.worker << " finishes the week's work on '" << task.title << "'.\n";
    }
  }
  normalize();
  return out.str();
}
}
