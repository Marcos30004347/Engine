#include "Company.hpp"

#include <algorithm>
#include <sstream>
#include <stdexcept>

namespace company {
namespace {
Decision make(std::string id, std::string speaker, std::string text,
              std::initializer_list<const char*> labels) {
  Decision d{std::move(id), std::move(speaker), std::move(text), {}};
  for (auto label : labels) d.options.push_back({label});
  return d;
}
const char* recoveries[] = {"one Region", "cross-Region backup and restore", "pilot light", "warm standby", "active/active"};
}
std::string Person::description() const {
  return name + (human() ? " | human, age " : " | AI, activated ") + std::to_string(age)
    + (human() ? " years | " : " months ago | ") + experience + " | technical "
    + std::to_string(technical) + "/100, contextual " + std::to_string(context)
    + "/100 | $" + std::to_string(monthlyCost) + "/month" + (human() ? " | morale " + std::to_string(morale) + "/100" : "");
}
static Person candidateTemplate(int index) {
  switch (index) {
  case 0: return {"Maya Chen", "8 years in production engineering; volunteers at a cinema", 34, -1, 78, 92, 6200};
  case 1: return {"Leo Costa", "1 year in support; knows the local film club", 23, -1, 40, 75, 3000};
  case 2: return {"Pip", "Sparrow model; 6 months of coding tasks; needs close review", 6, 0, 42, 18, 450};
  case 3: return {"Ada", "Forge model; 18 months of deployments; benefits from review", 18, 1, 78, 38, 2200};
  case 4: return {"Atlas", "Summit model; 30 months of verified migrations; still needs context", 30, 2, 96, 55, 8500};
  case 5: return {"Nova", "Prism model; newly released, strong coding with limited business context", 0, 1, 85, 40, 1300};
  case 6: return {"Orion", "Summit Next model; newly released, audited coding evaluations", 0, 2, 98, 62, 11000};
  default: throw std::invalid_argument("Unknown candidate");
  }
}
Game::Game(uint32_t seed, Business business, Difficulty difficulty) : rng(seed) {
  s.storySeed = seed;
  s.developmentWeeks = difficulty == Difficulty::Hard ? 12 : difficulty == Difficulty::Easy ? 24 : 18;
  s.business = business; s.scenarioCursor = seed % 8;
  if (business == Business::Game) s.rivalName = "Brightforge";
  const char* names[] = {"Afterglow", "Small Hours", "Velvet Frame", "Moonbeam"};
  s.companyName = names[roll(4)];
  if (business == Business::Game) s.companyName += " Games";
  refreshCandidates();
  setupDesign();
  setupProject();
}
void Game::refreshCandidates() {
  std::vector<std::string> previous{s.applicants[0].name,s.applicants[1].name};
  const char* first[] = {"Amara", "Rafael", "Nora", "Jun", "Sofia", "Ibrahim", "Clara", "Mateo", "Priya", "Theo", "Elena", "Noah"};
  const char* last[] = {"Silva", "Okafor", "Park", "Patel", "Costa", "Reed", "Sato", "Morales", "Chen", "Martin", "Ali", "Novak"};
  for (int i = 0; i < 7; ++i) {
    s.applicants[i] = candidateTemplate(i);
    if (i < 2) {
      auto& p = s.applicants[i];
      do { p.name = std::string(first[roll(12)]) + " " + last[roll(12)]; }
      while ((i > 0 && p.name == s.applicants[0].name) ||
        std::any_of(s.team.begin(), s.team.end(), [&](const Person& old) { return old.name == p.name; }) ||
        std::find(previous.begin(), previous.end(), p.name) != previous.end());
      const int experience = i == 0 ? 7 + roll(9) : 1 + roll(4);
      p.age = 20 + experience + roll(10);
      p.technical = i == 0 ? 70 + roll(24) : 35 + roll(26);
      p.context = 72 + roll(25);
      p.monthlyCost = ((2400 + experience * 230 + p.technical * 18) / 100) * 100;
      p.experience = std::to_string(experience) + (i == 0 ? " years in backend and platform engineering" : " years in customer operations and product support");
      s.prices[i] = p.monthlyCost;
    }
  }
  s.availableAfter[0] = s.availableAfter[1] = 0;
}
int Game::roll(int bound) { return static_cast<int>(rng() % static_cast<uint32_t>(bound)); }
void Game::normalize() {
  s.customers = std::clamp(s.customers, 0, 1000000);
  s.trust = std::clamp(s.trust, 0, 100);
  s.quality = std::clamp(s.quality, 0, 100);
  int total = 0;
  for (size_t i = 0; i < 7 && i < s.tasks.size(); ++i) total += s.tasks[i].progress;
  s.progress = total / 7;
  if (s.phase == Phase::Development) s.customers = 0;
  s.peakCustomers = std::max(s.peakCustomers, s.customers);
}
std::string Game::speaker() const { return s.team.empty() ? "You, founder and entire on-call rotation" : s.team.front().name; }
void Game::spend(int amount) { s.cash -= amount; s.incidentCost += amount; }
void Game::commitPay(const Person& p) {
  if (!s.weekOpen || std::find(s.committedNames.begin(), s.committedNames.end(), p.name) != s.committedNames.end()) return;
  s.committedNames.push_back(p.name);
  (p.human() ? s.committedHumans : s.committedAgents) += (p.monthlyCost + 3) / 4;
}
std::string Game::phaseName() const {
  return s.phase == Phase::Development ? "DEVELOPMENT" : s.phase == Phase::Launch ? "LAUNCH" : "MAINTENANCE";
}
Ledger Game::forecast() const {
  Ledger l;
  for (const auto& p : s.team) (p.human() ? l.humans : l.agents) += (p.monthlyCost + 3) / 4;
  l.cloud = (cloudCost() + 3) / 4;
  l.licensing = (s.phase == Phase::Development) ? 0 : (900 + s.customers * 2 + 3) / 4;
  l.revenue = (s.phase == Phase::Development) ? 0 : s.customers * 3;
  return l;
}
std::string Game::dashboard() const {
  return s.companyName + " | WEEK " + std::to_string(s.week) + " / 52 | " + phaseName()
    + "\nAvailable $" + std::to_string(s.cash) + " | recurring cost $" + std::to_string(forecast().costs())
    + "/week | customers " + std::to_string(s.customers)
    + " | employees " + std::to_string(std::count_if(s.team.begin(),s.team.end(),[](const Person& p) { return p.human(); }))
    + " | agents " + std::to_string(std::count_if(s.team.begin(),s.team.end(),[](const Person& p) { return !p.human(); }));
}
std::string Game::finances() const {
  const auto l = forecast();
  std::ostringstream out;
  out << "NEXT WEEK AT TODAY'S SCALE\nHuman salaries $" << l.humans
      << " | AI subscriptions $" << l.agents << " | cloud $" << l.cloud << " | content costs $" << l.licensing
      << "\nExpected app subscriptions $" << l.revenue << " | expected net $" << l.profit()
      << "\nOne-offs already paid since the last close: $" << s.incidentCost
      << "\nThis week's committed payroll: $" << s.committedHumans + s.committedAgents
      << " (worked weeks are payable even after dismissal)."
      << "\nCapital raised $" << s.investment << " | financing rounds " << s.rounds << "/3";
  if (l.profit() < 0) out << "\nRunway at this burn: about " << std::max(0, s.cash) / -l.profit() << " weeks, before surprises.";
  else out << "\nCurrent recurring revenue covers projected recurring costs.";
  out << "\nA game month is four weeks. Forecast excludes future incidents and investment.";
  return out.str();
}
std::string Game::introduction() const {
  return rule("intro").text + "\nPlan each Monday; measured delivery is reported monthly. Prices and characters are fictional. Press m for the desk, Ctrl+A / a for architecture, or an AWS label such as S.";
}

Decision Game::decision(const std::string& id) const {
  if(id == "architecture_review") {
    const auto finding=rule("review");
    std::string action=finding.action;
    for(const auto& binding:finding.bindings) {
      const auto token="{"+binding.first+"}"; size_t at;
      while((at=action.find(token))!=std::string::npos) action.replace(at,token.size(),binding.second);
    }
    if(action.empty()) throw std::logic_error("Architecture review has no follow-up decision");
    auto d=decision(action); d.speaker="Architecture review"; d.text=finding.text+"\n\n"+d.text;
    return d;
  }
  if(id == "design" || id.rfind("design:",0)==0) return designDecision(id);
  if (id.rfind("plan:", 0) == 0) {
    Decision d{id, "Monday planning", "What should " + id.substr(5) + " own this week?", {}};
    for (int task : availableTasks()) d.options.push_back({"T" + std::to_string(task + 1) + " | " + s.tasks[task].title});
    d.options.push_back({"Keep capacity free this week"});
    return d;
  }
  if (id.rfind("task:", 0) == 0) {
    const int task = std::stoi(id.substr(5));
    if (task < 0 || task >= static_cast<int>(s.tasks.size())) throw std::invalid_argument("Unknown project task");
    auto d = decision(implementationDecision(task)); d.id = id;
    for (const auto& a : s.assignments) if (a.task == task) d.speaker = a.worker;
    d.text = "While working on '" + s.tasks[task].title + "': " + d.text;
    return d;
  }
  if (id == "import_policy") return make(id, speaker(), "Our first beta users are signing up. Which account fields and usage events belong in the application and its diagnostic logs?",
    {"Collect only needed fields with consent and redact diagnostic logs | $300", "Copy everything into the app and full request logs"});
  if (id == "release") {
    Decision d{"release", "You", "Who leaves the team?", {}};
    for (const auto& p : s.team) d.options.push_back({p.name + (p.human() ? " | lay off; one week severance $" + std::to_string((p.monthlyCost + 3) / 4) : " | stop agent; no termination fee")});
    d.options.push_back({"Keep everybody"});
    return d;
  }
  if (id == "hire") {
    Decision d{"hire", "The recruitment inbox", "There is room for six colleagues. Who gets a seat at the folding table?", {}};
    for (int i = 0; i < s.marketSize; ++i) {
      auto p = candidate(i); p.monthlyCost = s.prices[i];
      d.options.push_back({p.description() + (s.availableAfter[i] > s.week ? " | unavailable until week " + std::to_string(s.availableAfter[i]) : "")});
    }
    d.options.push_back({"Keep the current team"});
    return d;
  }
  if (id == "scope") return make(id, speaker(),
    "Tuesday, at the whiteboard: what has to exist before somebody can watch a film?",
    {"Build a small catalogue, payments and dependable playback | $400",
     "Add social feeds, live chat and personalised trailers first | $1,000",
     "Rush a clickable demo into production | $100"});
  if (id == "launch") return make(id, speaker(),
    "The projector is ready. Outside, a few film-club members are waiting for a link. Is this opening night?",
    {"Open subscriptions to the public | requires the core APIs, onboarding and infrastructure to be delivered; $1,200",
     "Keep building and rehearse with the film club | $400"});
  if (id == "rest") return make(id, speaker(),
    "The last message in the team chat was sent at 2:13 a.m. The next one arrived at 6:02. How should this week feel?",
    {"Protect evenings and reduce the backlog | $250; reduce this week's scope",
     "Keep the current pace | no extra cost",
     "Push through another sprint | overtime this week"});
  if (id == "raise") return make(id, s.raiseName.empty() ? "The team" : s.raiseName,
    s.raiseName.empty() ? "There is no salary request pending." :
    "'I like what we are building. But my responsibilities have grown. Can we discuss a fifteen percent raise?'",
    {"Approve a 15% raise from next week", "Offer a workload reset instead | $250", "Decline the request"});
  if (id == "investment") return make(id, "Ruth, a local investor",
    "'Show me the runway and tell me why this company should exist.' You can pitch once every four weeks, for up to three rounds.",
    {"Pitch an angel round: $25,000 for 10 ownership points | $300 pitch cost",
     "Pitch a growth round: $70,000 for 25 ownership points | needs 400 customers and trust 60; $600 pitch cost",
     "Keep bootstrapping"});
  if (id == "storage") return make(id, speaker(),
    "The live catalogue is getting bigger. Where do the objects needed by customers live? Changes cost $900 once launched.",
    {"S3 Standard | $220 + $0.12/customer per month",
     "S3 Intelligent-Tiering, no optional archive tiers | $160 + $0.09/customer per month",
     "S3 Standard-IA for the whole live catalogue | $95 + $0.06/customer, plus retrieval charges",
     "S3 Glacier Deep Archive for the live catalogue | $25 + $0.01/customer, restore before use",
     "EBS gp3 attached to an EC2 origin in one AZ | $140 + $0.08/customer per month"});
  if (id == "scaling") return make(id, speaker(),
    "How much capacity may [A] consume? For Lambda this sets concurrency controls; for EC2/Fargate it sets replica policy.",
    {"Bounded demand scaling with load-tested limits | $120/month",
     "Fixed small capacity / low concurrency | $0 policy overhead",
     "Very high limits and aggressive scaling | $180/month before bursts",
     "Keep peak capacity warm all day | $3,200/month"});
  if (id == "database") return designDecision("design:database:D");
  if (id == "monitoring") return make(id, speaker(),
    "What should wake someone up before a small problem becomes a very large invoice?",
    {"CloudWatch alerts, redacted logs and AWS Budgets notifications | $140/month",
     "Full request logs including credentials, retained indefinitely | $35 base/month",
     "Minimal logs and no budget or service alarms | $0/month"});
  if (id == "rival") {
    const auto proposal = decision(s.intelDecision);
    Decision d{"rival", "An industry contact", rivalReport(), {}};
    d.options = {{"Adopt their approach: " + proposal.options[s.intelOption].label},
      {"Keep our design and investigate through the architecture desk"}, {"Compete on price: run a $3,000 campaign"}};
    return d;
  }
  if (id == "contract") return make(id, "A distribution partner",
    "A partner offers a large exclusive catalogue, but wants a non-refundable advance before seeing the audience figures.",
    {"Run a small $600 pilot with clear exit terms", "Pay the $18,000 advance and promise rapid growth", "Decline and keep the existing catalogue"});
  if (id == "promotion") return make(id, "The marketing chat",
    "A creator wants to send an audience your way this weekend. What happens if ten times as many people arrive?",
    {"Spend $450 on a bounded rehearsal and staged invitations", "Open the floodgates for $2,000", "Postpone the promotion"});
  if (id == "retention") return make(id, "The support inbox",
    "A loyal customer writes a long, thoughtful complaint. A refund would be cheaper than understanding it.",
    {"Spend $300 interviewing customers and fixing the rough edge", "Send a discount and move on | $800", "Close the ticket automatically"});
  if (id == "api") return designDecision("design:compute:A");
  if (id == "delivery") return make(id, speaker(),
    "A director sends a screener marked 'paying viewers only'. How does a subscriber watch it?",
    {"Private S3 + CloudFront OAC + signed viewer URLs | $180 + $0.65/customer monthly",
     "Public S3 object URLs embedded behind the login page | $40 + $1.20/customer monthly",
     "Proxy every video through the application servers | $420 + $1.85/customer monthly"});
  if (id == "identity") return make(id, speaker(),
    "The deployment script needs credentials. The first screening is getting closer, and the green deploy button is still grey.",
    {"Scoped IAM roles, short-lived credentials, separate production approval | $600 setup",
     "Give the deploy agent an administrator access key; store it in the project config",
     "Use one shared administrator login for everyone on the team"});
  if (id == "recovery") return make(id, speaker(),
    "The board asks what happens if your AWS Region vanishes. Include video, customer data, capacity, and traffic routing in the plan.",
    {"Stay in one Region; local backups | no additional monthly cost",
     "Copy backups across Regions; rebuild from infrastructure as code | +$250/month",
     "Pilot light: replicated data, provision application capacity on recovery | +$850/month",
     "Warm standby: replicated data and a small running stack | +$2,300/month",
     "Active/active: two serving Regions, replicated data and traffic failover | +$5,200/month"});
  if (id == "encoding" && s.business == Business::Game) return make(id, speaker(),
    "Persistent multiplayer sessions need a process running throughout the match, not just a login request.",
    {"ECS Fargate dedicated session tasks, across AZs | $700/month",
     "One standard Lambda invocation per entire long-running match | $80/month",
     "EC2 dedicated session fleet | $900/month"});
  if (id == "encoding") return make(id, speaker(),
    "The festival delivers feature films. Each encode needs forty minutes of continuous computation.",
    {"S3 queue + MediaConvert jobs | $350/month",
     "Put the entire forty-minute encode in one standard Lambda invocation | $80/month",
     "Keep a dedicated EC2 encoder running day and night | $1,650/month"});
  if (id == "review") return make(id, "You", "Who signs off production changes when the agent says 'done'?",
    {"Founder or human reviewer checks tests, permissions and intent before release | slower delivery",
     "Let agents ship autonomously when their own checks pass | faster delivery"});
  if (id == "drill") return make(id, speaker(),
    "The recovery document says 'switch over'. Nobody has tried it. Spend a rehearsal evening?",
    {"Restore data, exercise routing and test the application | $1,600",
     "Read the runbook aloud over pizza | $100",
     "The backup dashboard is green. Ship the next feature."});
  if (id == "context") return make(id, speaker(),
    "A grieving filmmaker asks to pause promotion of her comedy. The recommendation agent sees excellent click-through rates.",
    {"Call her, listen, and pause the campaign | $700 in lost promotion",
     "Ask the team to decide within their own judgement",
     "Keep the campaign running; the engagement numbers are compelling"});
  if (id == "growth") return make(id, speaker(),
    "A critic calls your catalogue 'a refuge from endless scrolling'. New viewers are arriving. What do you promise them?",
    {"A careful collection and honest release dates | $900 editorial work",
     "Every film, everywhere, immediately | $4,000 campaign",
     "Quietly improve playback and let subscribers bring friends | $600"});
  if (id == "incident") return make(id, "The support inbox", "After " + s.lastIncident + ", subscribers want an explanation.",
    {"Publish an honest timeline, repair the cause and give credits | $2,000",
     "Ask an agent to send cheerful reassurance; leave the system as it is | $150",
     "Fix the underlying issue quietly, with no customer update | $1,100"});
  if (id == "technical_question") {
    const char* prompts[] = {
      "'The new recommendation endpoint logs a whole request for debugging. Do we include the authorization header?'",
      "'The permission test failed. Can I give the preview worker access to every production bucket?'",
      "'The build works locally. Should I copy my administrator credential into the deployment image?'"
    };
    return make(id, speaker(), prompts[s.week % 3],
      {"Keep credentials private, scope access to this task, and verify with a test | $200",
       "Approve the proposed shortcut so the release can go ahead"});
  }
  if (id == "context_question") {
    const char* prompts[] = {
      "'A director asked us to remove a film in one country. Does delete mean remove it from the entire catalogue?'",
      "'A licensing email says schools may screen this film. Does that include our paying viewers at home?'",
      "'A festival partner says the poster is sensitive. Should I replace it without asking what they mean?'"
    };
    return make(id, speaker(), prompts[s.week % 3],
      {"Talk to the people involved and confirm the scope before changing anything | $150",
       "Trust the worker's first interpretation and move on"});
  }
  throw std::invalid_argument("Unknown decision: " + id);
}
std::string Game::choose(const Decision& d, size_t option) {
  const auto valid = decision(d.id);
  if (option >= valid.options.size()) throw std::invalid_argument("Choice out of range");
  const int c = static_cast<int>(option);
  if(d.id == "design" || d.id.rfind("design:",0)==0) return chooseDesign(d,option);
  if (d.id.rfind("plan:", 0) == 0) {
    if (!s.weekOpen || s.workPrepared) throw std::logic_error("Planning is only available before implementation begins");
    const auto who = d.id.substr(5);
    const auto people = planners();
    if (std::find(people.begin(), people.end(), who) == people.end()) return "That colleague is no longer available. The work remains on the board.";
    if (std::any_of(s.assignments.begin(), s.assignments.end(), [&](const Assignment& a) { return a.worker == who; }))
      return "That person already has an assignment for this week.";
    const auto tasks = availableTasks();
    if (option == tasks.size()) return who + " keeps some capacity free.";
    s.assignments.push_back({tasks[option], who, false});
    return who + " takes ownership of '" + s.tasks[tasks[option]].title + "'. The details will come up as the work unfolds.";
  }
  if (d.id.rfind("task:", 0) == 0) {
    const int task = std::stoi(d.id.substr(5));
    for (auto& assignment : s.assignments) if (assignment.task == task) {
      const auto active = planners();
      if (std::find(active.begin(), active.end(), assignment.worker) == active.end()) return "The owner has left. This task waits for a new assignment.";
      choose(decision(implementationDecision(task)), option);
      s.tasks[task].designMade = true; assignment.needsAnswer = false; ++s.questions;
      return assignment.worker + " notes the instruction and returns to the implementation.";
    }
    return "This task has no owner this week.";
  }
  if (d.id == "import_policy") {
    if (c == 0) { spend(300); s.monitoring = 0; } else s.monitoring = 1;
    return "The onboarding and data-retention policy is recorded. Implementation continues.";
  }

  if(d.id=="api" || d.id=="database") {
    return chooseDesign(designDecision(d.id=="api" ? "design:compute:A" : "design:database:D"),option);
  }
  std::string result;
  int* setting = nullptr;
  if (d.id == "api") setting = &s.api;
  else if (d.id == "delivery") setting = &s.delivery;
  else if (d.id == "storage") setting = &s.storage;
  else if (d.id == "scaling") setting = &s.scaling;
  else if (d.id == "database") setting = &s.database;
  else if (d.id == "monitoring") setting = &s.monitoring;
  else if (d.id == "encoding") setting = &s.encoding;
  else if (d.id == "recovery") setting = &s.recovery;
  if (setting && *setting != c && s.phase != Phase::Development) {
    spend(900); ++s.architectureChanges; s.lastMigrationWeek = s.week;
  }
  if (d.id == "storage" || d.id == "scaling" || d.id == "database" || d.id == "monitoring") {
    *setting = c;
    return "The architecture board is updated. The next workload will run against this configuration.";
  }
  if (d.id == "rival") {
    if (c == 0) return choose(decision(s.intelDecision), s.intelOption);
    if (c == 2) { spend(3000); s.customers += 80; normalize(); return "You buy attention. The underlying cost per customer has not changed."; }
    return "You keep the report. The node labels on the architecture board are a good place to investigate.";
  }
  if (d.id == "contract") {
    spend(c == 0 ? 600 : c == 1 ? 18000 : 0);
    s.customers += s.phase == Phase::Development ? 0 : c == 1 ? 120 : c == 0 ? 35 : 0;
    s.trust += c == 1 ? -3 : 1; normalize();
    return c == 1 ? "The catalogue grows overnight. The advance leaves the account just as quickly. Popularity still has to follow." :
      "You leave the meeting with a smaller commitment and enough room to change your mind.";
  }
  if (d.id == "promotion") {
    spend(c == 0 ? 450 : c == 1 ? 2000 : 0);
    if (c == 0) return "The rehearsal exposes the capacity limits before the invitation goes public. The architecture desk can help you change them.";
    if (c == 2) return "You move the event to another weekend.";
    const auto impact = assessArchitecture("surge");
    spend(impact.cost); s.lostDays += impact.days; s.trust -= impact.trust;
    s.incidentThisWeek = impact.days > 0; if (impact.days) ++s.outages;
    if (s.phase != Phase::Development) s.customers += impact.days ? 15 : 130;
    normalize(); return impact.text;
  }
  if (d.id == "retention") {
    spend(c == 0 ? 300 : c == 1 ? 800 : 0);
    s.trust += c == 0 ? 5 : c == 1 ? 1 : -9; s.quality += c == 0 ? 3 : 0; normalize();
    return c == 0 ? "The customer shows you a problem no dashboard could explain. A small fix earns a surprisingly warm reply." :
      "The ticket disappears from the queue. That is not quite the same as solving the problem.";
  }
  if (d.id == "release") {
    if (option == s.team.size()) return "Everyone stays.";
    const auto p = s.team[option];
    if (p.human()) spend((p.monthlyCost + 3) / 4);
    for(int i=0;i<7;++i) if(s.applicants[i].name==p.name) s.availableAfter[i]=s.week+2;
    result = p.name + (p.human() ? " packs the film poster into a cardboard tube. Severance is paid now; the worked week remains payable." :
      " finishes the current task and goes quiet. Next week's subscription stops; this week's committed charge remains.");
    if (s.raiseName == p.name) s.raiseName.clear();
    s.team.erase(s.team.begin() + c);
  } else if (d.id == "hire") {
    if (c == s.marketSize) return "The spare chair remains a very affordable colleague.";
    auto person = candidate(c); person.monthlyCost = s.prices[c]; person.nextReviewWeek = s.week + 52;
    if (s.availableAfter[c] > s.week) return person.name + " is not available yet. A new contract cannot erase the last conversation.";
    for (const auto& existing : s.team)
      if (existing.name == person.name) return person.name + " already works here. Nobody gets cloned this quarter.";
    if (s.team.size() >= 6) return "All six seats are occupied. Use the team menu to release a colleague first.";
    s.team.push_back(person); commitPay(person);
    result = person.name + (person.human() ? " puts a favourite film poster above the desk." : " joins the chat and asks what the popcorn emoji means.");
  } else if (d.id == "scope") {
    spend(c == 0 ? 400 : c == 1 ? 1000 : 100);
    s.projectScope = c;
    s.quality += c == 0 ? 6 : c == 1 ? 1 : -12;
    if (c == 2) s.scopedIdentity = false;
    result = c == 0 ? "By Friday, a film plays from beginning to end. There is very little to click, which turns out to be a relief." :
      c == 1 ? "The whiteboard fills with delightful ideas. The play button is still a drawing." :
      "The demo looks alive. Behind it, temporary permissions and skipped checks become tomorrow's work.";
  } else if (d.id == "launch") {
    if (s.phase != Phase::Development) return "The doors are already open.";
    if (c == 0 && !readyToLaunch()) return "The payment-to-playback path is unfinished. The film club agrees to wait.";
    if (c == 0) { s.phase = Phase::Launch; spend(1200); result = "You send the link. For a few seconds, nobody speaks. Then the first payment notification arrives."; }
    else { spend(400); s.quality += 5; result = "The film club finds a broken subtitle switch. Better ten patient friends than a thousand disappointed strangers."; }
  } else if (d.id == "rest") {
    if (c == 0) spend(250);
    for (auto& p : s.team) if (p.human()) p.morale = std::clamp(p.morale + (c == 0 ? 16 : c == 2 ? -14 : 0), 0, 100);
    result = c == 0 ? "Someone logs off while it is still light outside. On Friday, the conversation is about films again." :
      c == 1 ? "The calendar stays full. Nobody adds a smiley face to the meeting invitation." : "Tickets close quickly. The humans stop talking about their weekends.";
  } else if (d.id == "raise") {
    for (auto& p : s.team) if (p.name == s.raiseName) {
      if (c == 0) { p.monthlyCost = (p.monthlyCost * 115 + 99) / 100; p.morale = std::min(100, p.morale + 20); }
      else if (c == 1) { spend(250); p.morale = std::min(100, p.morale + 10); }
      else p.morale = std::max(0, p.morale - 24);
    }
    result = s.raiseName.empty() ? "Nobody has a salary request pending." : c == 0 ?
      s.raiseName + " exhales. 'Thank you. I wanted to stay.' The new salary starts next week." : c == 1 ?
      "You remove two responsibilities from the rota. The money question is postponed, not forgotten." :
      "'I understand,' comes the answer. It does not sound like agreement.";
    s.raiseName.clear();
  } else if (d.id == "investment") {
    if (c == 2) return "Ruth closes the notebook. 'The door is open when you need it.'";
    if (s.rounds >= 3 || s.week - s.lastPitchWeek < 4) return "The investors want four weeks of new evidence between pitches, and offer at most three rounds.";
    if (c == 1 && (s.customers < 400 || s.trust < 60)) return "Ruth needs 400 paying customers and trust of at least 60 before considering that growth round.";
    const int equity = c == 0 ? 10 : 25;
    if (s.founderEquity < equity) return "You no longer have enough ownership to offer those terms.";
    s.lastPitchWeek = s.week; spend(c == 0 ? 300 : 600);
    if (roll(100) >= (c == 0 ? 80 : 65)) return "Ruth likes the idea, but her partners pass this time. The pitch costs are real; the promised cheque was not.";
    const int amount = c == 0 ? 25000 : 70000;
    s.cash += amount; s.investment += amount; s.founderEquity -= equity; ++s.rounds;
    result = "The investment lands: $" + std::to_string(amount) + ". You still run the company, but now own " + std::to_string(s.founderEquity) + "% of it. Capital is not sales revenue.";
  } else if (d.id == "api") { s.api = c; result = "The subscription service gets a home. The bill will arrive even on quiet days.";
  } else if (d.id == "delivery") { s.delivery = c; result = "The screener loads. The director sends a relieved thumbs-up.";
  } else if (d.id == "identity") { s.scopedIdentity = c == 0; if (c == 0) spend(600); result = "The deploy button turns green. Permissions become part of your company's history.";
  } else if (d.id == "recovery") { if (s.recovery != c) s.testedRecovery = false; s.recovery = c; result = std::string("The architecture diagram now says ") + recoveries[c] + ". A diagram is a promise to your future self.";
  } else if (d.id == "encoding") { s.encoding = c; result = "The films enter the encoding queue. Everyone waits for the first finished frame.";
  } else if (d.id == "review") { s.reviewed = c == 0; result = c == 0 ? "Changes wait for a second pair of eyes. Some tickets take longer." : "The agent receives the production release button. Tickets move quickly.";
  } else if (d.id == "drill") {
    spend(c == 0 ? 1600 : c == 1 ? 100 : 0);
    s.testedRecovery = c == 0;
    result = c == 0 ? "The rehearsal catches a missing DNS permission. You fix it before anybody is watching." : "The runbook stays theoretical. The pizza, if any, does not.";
  } else if (d.id == "context") {
    int context = 20;
    for (const auto& p : s.team) context = std::max(context, p.context);
    if (c == 0 || (c == 1 && roll(100) < context)) {
      s.trust += 10; spend(700);
      result = "You pause the campaign. 'Thank you for remembering there is a person behind the film,' she writes.";
    } else { s.trust -= 16; result = "The cheerful campaign runs beside an obituary. A technically perfect recommendation becomes a very human mistake."; }
  } else if (d.id == "growth") {
    if (s.phase == Phase::Development) return "There is no public product to promote yet.";
    spend(c == 0 ? 900 : c == 1 ? 4000 : 600);
    s.customers += c == 1 ? 150 : 50;
    s.trust += c == 1 ? -12 : 4;
    s.quality += c == 2 ? 7 : 0;
    result = c == 1 ? "The campaign brings a crowd. Support starts receiving requests for films you have never licensed." : "A subscriber brings three friends to a watch party. Growth has names, for once.";
  } else if (d.id == "incident") {
    spend(c == 0 ? 2000 : c == 1 ? 150 : 1100);
    if (c != 1) { s.scopedIdentity = true; s.delivery = 0; if (s.database == 2) s.database = 0; for(auto& db:s.design.databases) if(db.service==2) db.service=0; if (s.monitoring == 1) s.monitoring = 0; s.reviewed = true; s.trust += c == 0 ? 12 : 2; }
    else s.trust -= 8;
    result = c == 0 ? "The explanation is uncomfortable. The replies are kinder than expected. Permissions and delivery controls are repaired." : c == 1 ? "'We value your trust,' says the email. The same unresolved configuration waits behind it." : "The credentials and delivery controls are repaired. Subscribers are left to guess what happened.";
  } else if (d.id == "technical_question") {
    ++s.questions;
    if (c == 0) spend(200);
    else s.scopedIdentity = false;
    result = c == 0 ? "The change keeps credentials private and permissions scoped. A small interruption buys a quieter night." : "The shortcut ships. The release dashboard has nothing to say about the permissions.";
  } else if (d.id == "context_question") {
    ++s.questions;
    if (c == 0) { spend(150); s.trust += 2; }
    const auto concern = rule("partner");
    result = c == 0 ? "A short conversation confirms the request before the worker changes anything." : "The interpretation is recorded for the next content review.";
    if (!concern.id.empty()) {
      result += "\n" + concern.text;
      s.trust -= concern.trust;
    }

  }
  normalize();
  return result;
}
std::string Game::beginWeek() {
  if (s.weekOpen) throw std::logic_error("Close the current week first");
  ++s.week; updateIntel(); s.assignments.clear(); s.workPrepared = false; s.workResolved = false; s.weekOpen = true; s.incidentThisWeek = false;
  if (s.phase == Phase::Launch && s.launchProcessed) s.phase = Phase::Maintenance;
  s.committedHumans = 0; s.committedAgents = 0; s.committedNames.clear();
  for (const auto& p : s.team) commitPay(p);
  std::string text = rule("monday", "Company configured scene"+std::to_string(roll(4))+"\n").text;
  if (s.phase == Phase::Development) text += "\nThere are no paying customers yet. Pick the API boundaries, build the smallest useful product, and keep the AWS bill inside the runway.";
  else text += "\n" + std::to_string(s.customers) + " people are paying to be here. After " + s.lastIncident + ", you want to give them a quieter week.";
  if (s.lostDays > 0) text += "\nRecovery is still underway: " + std::to_string(s.lostDays) + " days of disruption remain.";
  if(s.phase==Phase::Development) text += "\nDevelopment window: " + std::to_string(s.developmentWeeks/4) + " months; " + std::to_string(std::max(0,s.developmentWeeks-(s.week-s.developmentStart))) + " weeks remain. Finishing early allows an earlier launch.";
  return text;
}
Decision Game::chapter() const {
  const auto selected = rule("chapter");
  if (selected.id.empty()) throw std::logic_error("Company rules did not select a chapter");
  return decision(selected.action);
}
std::vector<Decision> Game::questions() const {
  std::vector<Decision> result;
  for (const auto& a : s.assignments) if (a.needsAnswer) result.push_back(decision("task:" + std::to_string(a.task)));
  return result;
}
std::string Game::event() {
  if (!s.weekOpen) throw std::logic_error("Begin a week before generating events");
  std::ostringstream out;
  out << "Thursday. ";
  const auto deadline=rule("deadline");
  if(!deadline.id.empty()) { spend(deadline.cost); out << deadline.text << '\n'; }
  const auto market=rule("market");
  if(!market.id.empty()) {
    out << market.text << '\n';
    if(market.action=="model6" || market.action=="model7") {
      s.marketSize=market.action=="model6"?6:7;
      const auto p=candidate(s.marketSize-1);
      out << "A model release: " << p.description() << '\n';
    } else if(market.action=="price") {
      const int model=2+roll(s.marketSize-2);
      s.prices[model]=(s.prices[model]*120+99)/100;
      out << candidate(model).name << " raises its subscription by 20%, to $" << s.prices[model] << "/month.\n";
      for(auto& p:s.team) if(p.name==candidate(model).name) p.monthlyCost=s.prices[model];
    }
  }
  const auto scene=rule("week","Company configured scene"+std::to_string(roll(8))+"\n");
  out << scene.text << '\n';
  spend(scene.cost); s.lostDays+=scene.days; s.trust-=scene.trust;
  if(scene.action=="aliens") { s.lastIncident="the Great Regional Abduction"; out << "The Great Regional Abduction enters the company chronicle.\n"; }
  if(scene.days) { ++s.outages; s.incidentThisWeek=true; }
  if(s.phase==Phase::Launch && !s.launchProcessed) {
    s.launchProcessed=true;
    s.customers=100+s.quality*5+s.trust*2;
    s.lastIncident="opening night";
    const auto impact=assessArchitecture("surge");
    s.lostDays+=impact.days; s.trust-=impact.trust;
    if(impact.days) { ++s.outages; s.incidentThisWeek=true; out << impact.text << '\n'; }
    if(s.encoding==1) { s.customers/=2; s.quality-=12; out << assessArchitecture("jobs").text << '\n'; }
  } else if(s.phase==Phase::Development) {
    const auto impact=assessArchitecture("jobs");
    if(impact.days) { s.quality-=4; out << impact.text << '\n'; }
  } else if(s.phase==Phase::Maintenance && scene.action!="aliens") {
    if(!scene.action.empty()) {
      const auto impact=assessArchitecture(scene.action);
      spend(impact.cost); s.lostDays+=impact.days; s.trust-=impact.trust;
      if(impact.breach) { ++s.breaches; s.customers=s.customers*85/100; }
      if(impact.days) ++s.outages;
      if(impact.days || impact.breach || impact.cost) { s.incidentThisWeek=true; s.lastIncident=scene.action; }
      out << impact.text << '\n';
    }
    out << architectureEvent();
    const auto topology=rule("topology");
    spend(topology.cost); s.lostDays+=topology.days; s.trust-=topology.trust;
    if(topology.days) ++s.outages;
    if(topology.breach) { ++s.breaches; s.customers=s.customers*85/100; }
    if(topology.days || topology.cost || topology.breach) { s.incidentThisWeek=true; s.lastIncident="an API/backend cutover"; }
    out << topology.text << '\n';
    // The partner reacts to an actual connected architecture pattern only.
    if(s.week%4==0) { const auto concern=rule("partner"); if(!concern.id.empty()) { out << concern.text << '\n'; s.trust-=concern.trust; } }
  }
  // A named request remains pending until answered or the employee leaves.
  if (s.raiseName.empty()) {
    for (auto& p : s.team) if (p.human() && s.week >= p.nextReviewWeek) {
      s.raiseName = p.name; p.nextReviewWeek = s.week + 52;
      out << p.name << " has reached the annual salary review. A private conversation is on Friday's calendar.\n"; break;
    }
  }
  normalize();
  return out.str();
}
std::string Game::closeWeek() {
  if (!s.weekOpen) throw std::logic_error("No open week to close");
  const int down = std::min(s.lostDays, 7);
  if (s.phase == Phase::Launch || s.phase == Phase::Maintenance) {
    s.customers += s.customers * (s.trust - 55) / 1600 + s.quality / 5 + roll(21) - 10;
    s.customers -= s.customers * down / 50;
  }
  if (s.phase != Phase::Development) {
    s.rivalCustomers = std::max(s.rivalCustomers + 35, s.customers / 2);
  }
  normalize();
  s.ledger = forecast();
  s.ledger.revenue = (s.phase == Phase::Development) ? 0 : s.customers * 3 * (7 - down) / 7;
  s.ledger.humans = s.committedHumans; s.ledger.agents = s.committedAgents;
  s.ledger.incidents = s.incidentCost;
  // One-offs are already deducted when chosen. Financing is never revenue.
  s.cash += s.ledger.profit() + s.ledger.incidents;
  s.monthlyRevenue += s.ledger.revenue; s.monthlyCosts += s.ledger.costs();
  s.incidentCost = 0; s.lostDays -= down; s.weekOpen = false;
  std::ostringstream out;
  out << "FRIDAY, AFTER THE LAST EMAIL\n";
  for (auto it = s.team.begin(); it != s.team.end();) {
    if (it->human()) {
      it->morale = std::clamp(it->morale - (s.team.size() == 1 ? 4 : 1) - down * 2 - (s.incidentThisWeek ? 5 : 0), 0, 100);
      if (it->morale <= 20 || (it->morale < 45 && roll(100) < 18)) {
        out << it->name << " leaves a resignation letter beside the keyboard. 'I wanted this to work. I need my evenings back.'\n";
        for(int i=0;i<2;++i) if(s.applicants[i].name==it->name) s.availableAfter[i]=s.week+8;
        if (s.raiseName == it->name) s.raiseName.clear();
        ++s.resignations; it = s.team.erase(it); continue;
      }
    } else if (s.week % 4 == 0) ++it->age;
    ++it;
  }
  out << "App subscriptions brought $" << s.ledger.revenue << "; total operating costs were $" << s.ledger.costs()
      << ". The difference is $" << s.ledger.profit() << ".\n"
      << "Human pay $" << s.ledger.humans << " | agents $" << s.ledger.agents << " | cloud $" << s.ledger.cloud
      << " | content costs $" << s.ledger.licensing << " | one-offs $" << s.ledger.incidents << "\n"
      << "Available money: $" << s.cash << ". Customers: " << s.customers << ".\n";
  if (s.phase == Phase::Development) out << "No subscription revenue yet. The team closes its laptops with another piece of the system in place.\n";
  else if (s.ledger.profit() < 0) out << "You underline the cash balance, then close the notebook. There are still choices to make.\n";
  else out << "The subscriptions paid for the week. Someone closes the AWS console and suggests going home.\n";
  if (s.week % 4 == 0) {
    s.lastMonthlyReport = "MONTHLY REVIEW / month " + std::to_string(s.week / 4) +
      "\nSystem delivery: " + std::to_string(s.progress) + "% | playback quality " + std::to_string(s.quality) +
      "/100 | customer trust " + std::to_string(s.trust) + "/100\n" + taskBoard(true) +
      "\nRevenue $" + std::to_string(s.monthlyRevenue) + " | costs $" + std::to_string(s.monthlyCosts) + "\n\n" + architectureReport();
    out << "\n" << s.lastMonthlyReport << "\n";
    s.monthlyRevenue = 0; s.monthlyCosts = 0;
  }
  return out.str();
}
std::string Game::ending() const {
  std::ostringstream out;
  if (bankrupt()) out << "THE FINAL SCREENING\nThe money reaches the credits before the company does. You keep the film poster and the lessons.\n";
  else if (s.phase == Phase::Development) out << "STILL ON THE WHITEBOARD\nFifty-two weeks, and the doors never opened. A prototype survives; a business still needs an audience.\n";
  else out << "RENEWED FOR ANOTHER SEASON\n" << s.companyName << " reaches the end of its first 52 weeks. "
    << (s.ledger.profit() >= 0 ? "The last week pays its own way.\n" : "The next chapter still needs a plan for the burn.\n");
  out << "Weeks " << s.week << " | cash $" << s.cash << " | customers " << s.customers << " | peak " << s.peakCustomers
      << "\nBreaches " << s.breaches << " | outages " << s.outages << " | resignations " << s.resignations
      << " | questions " << s.questions << "\nFounder ownership " << s.founderEquity << "% | capital raised $" << s.investment
      << "\nRival " << s.rivalName << ": " << s.rivalCustomers << " customers. Architecture revisions: " << s.architectureChanges
      << "\nClosing architecture: " << recoveries[s.recovery] << (s.testedRecovery ? ", rehearsed.\n" : ", unrehearsed.\n")
      << "Losing is a story, too. Try the same seed with different decisions.\n";
  return out.str();
}
}
