#include "Company.hpp"
#include <iostream>
#include <algorithm>
#include <stdexcept>

namespace {
void check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
std::string choose(company::Game& g, const char* id, size_t option) { return g.choose(g.decision(id), option); }
void plan(company::Game& g) {
  for (const auto& who : g.planners()) choose(g, ("plan:"+who).c_str(), 0);
  g.prepareWork();
  for (const auto& q : g.questions()) g.choose(q, 0);
  g.resolveWork();
}
void build(company::Game& g) {
  for (int i = 0; i < 25 && !g.readyToLaunch(); ++i) {
    g.beginWeek(); plan(g); choose(g,"rest",0); g.closeWeek();
  }
  check(g.readyToLaunch(), "Assigned core work must become launchable");
}
std::string play(company::Game& g) {
  std::string story = g.introduction();
  choose(g, "hire", 3);
  for (int i = 0; i < company::Game::campaignWeeks && !g.bankrupt(); ++i) {
    story += g.beginWeek();
    for (const auto& who : g.planners()) story += choose(g, ("plan:"+who).c_str(), 0);
    story += g.prepareWork();
    auto d = g.chapter();
    story += g.choose(d, d.id == "recovery" ? 2 : 0);
    for (const auto& q : g.questions()) story += g.choose(q, 0);
    story += g.resolveWork();
    story += g.event();
    if (!g.state().raiseName.empty()) story += choose(g, "raise", 0);
    if (g.state().incidentThisWeek) story += choose(g, "incident", 0);
    story += g.closeWeek();
  }
  return story + g.ending();
}
void launch(company::Game& g) {
  build(g);
  g.beginWeek();
  choose(g, "launch", 0); g.event(); g.closeWeek();
}
void beforeWeek(company::Game& g, int week) {
  while (g.state().week < week - 1) { g.beginWeek(); g.closeWeek(); }
  g.beginWeek();
}
void rewriteRules() {
  company::Game safe(7);
  check(safe.rule("partner").id.empty(), "Safe graph must never upset partner");
  check(choose(safe,"context_question",1).find("upset")==std::string::npos, "Context answer alone must not upset partner");
  choose(safe,"delivery",1);
  const auto exposed=safe.rule("partner");
  check(exposed.id=="partner-public-catalogue" && exposed.text.find("[E -> S]")!=std::string::npos,
        "Complaint must match a connected delivery-to-storage pattern and explain it");
  choose(safe,"storage",4);
  check(safe.rule("partner").id=="partner-origin-mismatch", "An incompatible origin must not be misreported as a successful public download");
  choose(safe,"storage",0);
  choose(safe,"delivery",0);
  check(safe.rule("partner").id.empty(), "Repair must remove the matched complaint");
  choose(safe,"storage",3);
  check(safe.rule("partner").id=="partner-archived-premiere", "Live archive must cause a specific complaint");
  check(safe.rule("intro").text.find(safe.state().companyName)!=std::string::npos, "Narrator must interpolate graph-bound company name");
  check(safe.graphFacts().find("Edge calls Store")!=std::string::npos, "Decision graph must publish real topology");
  check(safe.systemGraph().find("SYSTEM MAP")!=std::string::npos && safe.systemGraph("4").find("Glacier Deep Archive")!=std::string::npos,
        "Two-level renderer must show current component settings");
  company::Game hard(4,company::Business::Game,company::Difficulty::Hard), easy(4,company::Business::Game,company::Difficulty::Easy);
  check(hard.state().developmentWeeks==12 && easy.state().developmentWeeks==24, "Difficulty controls development window");
  for(int i=0;i<12;++i) { hard.beginWeek(); hard.closeWeek(); easy.beginWeek(); easy.closeWeek(); }
  check(hard.rule("deadline").id.empty(), "Deadline must allow the full twelve weeks");
  hard.beginWeek(); easy.beginWeek();
  check(hard.rule("deadline").cost==1000 && easy.rule("deadline").id.empty(), "Only the overdue difficulty may incur delay cost");
  company::Game staff(8,company::Business::Game);
  check(!staff.hiringAdvice().empty(), "Independent backlog must proactively suggest hiring");
  const auto first=staff.candidate(0).name;
  choose(staff,"hire",0); staff.refreshCandidates();
  check(staff.candidate(0).name!=first && staff.state().team.front().name==first, "New hiring visit changes candidates, not current workers");
  choose(staff,"hire",0); staff.refreshCandidates(); choose(staff,"hire",0); staff.refreshCandidates(); choose(staff,"hire",0);
  check(staff.planners().size()==5, "Every additional hire adds a weekly planning slot");
  staff.beginWeek(); choose(staff,"release",0); staff.closeWeek();
  check(staff.state().team.size()==3, "Old employees remain dismissible after applicant refresh");
}
void editableTopology() {
  company::Game g(21,company::Business::Game);
  check(!g.designReady() && g.state().design.compute.front().service==-1 && g.state().design.databases.front().service==-1,
    "System starts with undecided server and database");
  check(g.chapter().id=="design", "High-level design must precede low-level refinement");
  choose(g,"design",0);
  check(g.chapter().id=="design:compute:A", "Refine the unresolved server after ownership");
  choose(g,"design:compute:A",1); choose(g,"design:database:D",0);
  check(g.designReady(), "Chosen primary services form a usable graph");
  const auto firstCost=g.cloudCost();
  choose(g,"design:route:U3",1); // one current compute; first extraction is Lambda
  check(g.state().design.apis[2].compute=="C2" && g.state().design.apis[0].compute=="A", "Moving payments cannot move accounts");
  check(g.state().design.apis[2].data==std::vector<std::string>({"B1","B3"}), "Extracting an API preserves dataset dependencies");
  choose(g,"design:add-database",0); choose(g,"design:database:D2",3);
  choose(g,"design:data:B3",2); // D2, verified cutover
  const auto edges=g.designEdges();
  check(std::find(edges.begin(),edges.end(),std::make_pair(std::string("C2"),std::string("D")))!=edges.end() &&
    std::find(edges.begin(),edges.end(),std::make_pair(std::string("C2"),std::string("D2")))!=edges.end(),
    "One Lambda must reach both member DynamoDB and invoice Aurora");
  check(g.state().design.data[0].database=="D" && g.state().design.data[2].database=="D2", "Only selected dataset migrates");
  check(g.cloudCost()>firstCost, "Dedicated compute and second database must both be billed");
  check(g.rule("topology").id=="topology-connections" && g.rule("topology").text.find("Payments [U3]")!=std::string::npos,
    "Narrated connection-pressure rule must identify actual API and backend");
  const auto review=g.decision("architecture_review");
  check(review.id=="design:pool:D2" && review.text.find("Payments [U3]")!=std::string::npos,
    "Architecture review must bind its repair to the actual connected backend");
  const auto report=g.architectureReport();
  check(report.find("C2")!=std::string::npos && report.find("D2")!=std::string::npos && report.find("Payments")!=std::string::npos,
    "Architecture report must include split API dependencies");
  int nodeTotal=0; for(const auto& node:g.architectureNodes()) nodeTotal+=node.monthlyCost;
  check(nodeTotal==g.cloudCost() && report.find("TOTAL $"+std::to_string(nodeTotal)+"/month")!=std::string::npos,
    "Reported node charges must reconcile to the cloud bill without shared-service duplication");
  const auto history=g.state().designHistory.size(); const auto reportCash=g.state().cash;
  check(report==g.architectureReport() && reportCash==g.state().cash && history==g.state().designHistory.size(),
    "Reporting must be deterministic and read only");
  choose(g,"design:pool:D2",0);
  check(g.state().designHistory.size()==history+1, "A graph repair must be recorded in design history");
  choose(g,"design:pool:D2",0);
  check(g.state().designHistory.size()==history+1, "Repeated unchanged decisions must not create history");
  check(g.rule("topology").id=="topology-quiet", "Pooling must repair the matching connected pattern");
  choose(g,"design:data:B3",1); // quick reverse migration to DynamoDB
  check(g.state().design.data[2].database=="D" && g.rule("topology").id=="topology-query-model", "Relational cutover without access-pattern adaptation must leave causal migration risk");
  choose(g,"design:verify:B3",0);
  check(g.rule("topology").id=="topology-quiet", "Verified access-pattern adaptation supports DynamoDB too");
  choose(g,"design:data:B4",3);
  check(g.rule("topology").id=="topology-unverified", "An unreconciled key-value copy must report missing records, not SQL assumptions");
  choose(g,"design:verify:B4",0); choose(g,"design:data:B4",0);
  check(g.decision("architecture_review").id=="design:retire:D2",
    "Idle capacity review must target the emptied database");
  const auto bill=g.cloudCost(); choose(g,"design:retire:D2",0);
  check(g.cloudCost()<bill && g.state().design.databases.size()==1, "Unused database can retire and stop billing");
  const auto count=g.state().design.compute.size(); choose(g,"design:retire:C2",0);
  check(g.state().design.compute.size()==count, "Cannot retire a server while its API is routed there");
  choose(g,"design:route:U3",0); choose(g,"design:retire:C2",0);
  check(g.state().design.compute.size()==1, "Retiring an empty extracted server removes it");
  choose(g,"design:add-database",0); choose(g,"design:database:D3",2);
  check(g.rule("topology").id=="topology-quiet", "An unused database cannot expose data through a nonexistent API path");
  choose(g,"design:data:B1",2);
  check(g.rule("topology").breach && g.rule("topology").text.find("D3")!=std::string::npos, "Security incident must follow the newly connected backend");
  choose(g,"design:data:B1",0); choose(g,"design:retire:D3",0);
  for(const auto& edge:g.architectureEdges()) {
    bool source=false,target=false;
    for(const auto& node:g.architectureNodes()) { source|=node.label==edge.first; target|=node.label==edge.second; }
    check(source && target, "Topology rewrites cannot leave dangling physical edges");
  }
  bool rejected=false;
  try { choose(g,"design:route:missing",0); } catch(const std::invalid_argument&) { rejected=true; }
  check(rejected, "Unknown node edits must fail without creating dangling edges");
  company::Game split(6,company::Business::Game); choose(split,"design",1);
  check(split.state().design.apis[2].compute=="C2" && split.state().design.data[2].database=="D2", "High-level split keeps AWS selection undecided");
  choose(split,"design:compute:A",1); choose(split,"design:database:D",0); choose(split,"design:compute:C2",1); choose(split,"design:database:D2",3);
  // A payment task asks about its actual assigned compute node, not the shared server.
  check(split.decision("task:2").text.find("[C2]")!=std::string::npos, "Implementation questions follow API ownership");
  company::Game live(9); launch(live);
  auto cash=live.state().cash; choose(live,"design:route:U3",1);
  check(live.state().cash==cash-900, "Live API extraction charges a cutover once");
  cash=live.state().cash; choose(live,"design:route:U3",1);
  check(live.state().cash==cash, "Selecting the current owner must be free");
  choose(live,"design:add-database",0); choose(live,"design:database:D2",3);
  cash=live.state().cash; choose(live,"design:data:B3",2);
  check(live.state().cash==cash-1200, "Verified live dataset migration has its own cost");
}
void lifecycle() {
  company::Game g(3);
  check(g.state().customers == 0 && g.state().phase == company::Phase::Development && g.state().cash == 85000, "Must start as an unfunded-product startup with cash and no customers");
  choose(g, "launch", 0);
  check(g.state().phase == company::Phase::Development, "Cannot launch unfinished playback");
  g.beginWeek(); choose(g, "growth", 0); g.event(); g.closeWeek();
  check(g.state().customers == 0 && g.state().ledger.revenue == 0 && g.state().ledger.licensing == 0,
        "Development cannot create app subscriptions");
  launch(g);
  check(g.state().phase == company::Phase::Launch && g.state().customers > 0, "Launch must open subscriptions");
  g.beginWeek(); check(g.state().phase == company::Phase::Maintenance, "Next week must enter maintenance"); g.closeWeek();
  company::Game good(8), bad(8);
  build(good); build(bad);
  choose(bad, "api", 2); choose(bad, "delivery", 2); choose(bad, "encoding", 1);
  good.beginWeek(); bad.beginWeek(); choose(good,"launch",0); choose(bad,"launch",0);
  good.event(); bad.event(); good.closeWeek(); bad.closeWeek();
  check(good.state().ledger.revenue > bad.state().ledger.revenue && bad.state().outages > 0,
        "Development architecture must change launch outcomes");
}
void recovery() {
  int previousLoss = 100, previousBill = 0;
  for (size_t strategy = 0; strategy < 5; ++strategy) {
    company::Game g(3); launch(g);
    choose(g, "recovery", strategy); choose(g, "drill", 0);
    check(g.cloudCost() > previousBill, "Recovery cost must rise with preparedness"); previousBill = g.cloudCost();
    beforeWeek(g, 20);
    auto event = g.event();
    check(event.find("saucer") != std::string::npos, "Crisis must occur");
    int days = g.state().lostDays;
    check(days < previousLoss, "Better recovery must reduce downtime"); previousLoss = days;
    g.closeWeek();
    check(g.state().lostDays == std::max(0, days - 7), "Recovery must persist across week boundaries");
    if (days >= 7) check(g.state().ledger.revenue == 0, "A week offline earns no subscriptions");
  }
  company::Game a(3), b(3); launch(a); launch(b);
  choose(a, "recovery", 2); choose(b, "recovery", 2); choose(b, "drill", 0);
  beforeWeek(a, 20); beforeWeek(b, 20); a.event(); b.event();
  check(a.state().lostDays == b.state().lostDays + 3, "Rehearsal must reduce downtime");
}
void staffing() {
  company::Game g(9);
  choose(g, "hire", 0); choose(g, "hire", 2); choose(g, "hire", 2);
  check(g.state().team.size() == 2, "No duplicate people");
  g.beginWeek();
  const int cash = g.state().cash;
  const int pay = (g.state().team[0].monthlyCost + 3) / 4;
  choose(g, "release", 0);
  check(g.state().cash == cash - pay, "Human severance paid immediately");
  choose(g, "release", 0); g.closeWeek();
  check(g.state().ledger.humans == pay && g.state().ledger.agents == 113, "Worked week remains payable after dismissal");
  check(g.forecast().humans == 0 && g.forecast().agents == 0, "Released workers stop recurring charges");
  choose(g, "hire", 0); check(g.state().team.empty(), "Cannot immediately rehire to reset morale or salary");
  company::Game exhausted(9); choose(exhausted, "hire", 0);
  for (int i = 0; i < 5; ++i) { exhausted.beginWeek(); choose(exhausted, "rest", 2); exhausted.closeWeek(); }
  check(exhausted.state().resignations == 1 && exhausted.state().team.empty(), "Overworked humans must be able to quit");

}
void salaryAndMarket() {
  company::Game g(2); choose(g, "hire", 0); launch(g);
  choose(g, "recovery", 2); choose(g, "drill", 0);
  while (g.state().week < 51) {
    g.beginWeek(); choose(g, "rest", 0); g.event();
    check(g.state().raiseName.empty(), "Salary requests must not occur before annual tenure");
    g.closeWeek();
  }
  g.beginWeek(); g.event();
  check(g.state().raiseName == g.candidate(0).name, "Employee must ask for a raise");
  int pay = g.state().team[0].monthlyCost;
  choose(g, "raise", 0); g.closeWeek();
  check(g.state().ledger.humans == (pay + 3) / 4, "Raise must not retroactively change pay");
  check(g.forecast().humans > g.state().ledger.humans && g.state().raiseName.empty(), "Raise affects next week's payroll");
  company::Game models(2); choose(models, "hire", 2); choose(models, "hire", 3); choose(models, "hire", 4);
  beforeWeek(models, 7); auto oldPrices = models.state().prices;
  const int committed = models.state().committedAgents; models.event();
  check(oldPrices != models.state().prices, "Providers must raise prices");
  models.closeWeek();
  check(models.state().ledger.agents == committed, "Price notice cannot change already committed pay");
  check(models.forecast().agents > committed, "Renewal must use the new price");
  beforeWeek(models, 10); models.event();
  check(models.state().marketSize == 6 && models.decision("hire").options.size() == 7, "New model must enter hiring menu");
  choose(models, "release", 0); choose(models, "hire", 5);
  check(models.state().team.back().name == "Nova", "New model must actually be hireable");
  models.closeWeek(); beforeWeek(models, 18); models.event();
  check(models.state().marketSize == 7, "Second model release must enter market");
}
void financesAndInvestment() {
  company::Game g(1); choose(g, "hire", 3);
  const int before = g.state().cash;
  g.beginWeek(); choose(g, "scope", 0); g.event(); g.closeWeek();
  check(g.state().cash == before + g.state().ledger.profit(), "Weekly ledger must reconcile with immediate expenses");
  int funded = 0, rejected = 0;
  for (int seed = 0; seed < 30; ++seed) {
    company::Game pitch(seed); int cash = pitch.state().cash;
    choose(pitch, "investment", 1); check(pitch.state().cash == cash, "Unqualified growth pitch is not charged");
    choose(pitch, "investment", 0);
    check(pitch.state().cash == cash - 300 + pitch.state().investment, "Investment and pitch fee must reconcile");
    if (pitch.state().investment) { ++funded; check(pitch.state().founderEquity == 90, "Investment dilutes ownership"); }
    else ++rejected;
    int after = pitch.state().cash; choose(pitch, "investment", 0);
    check(pitch.state().cash == after, "Repeated pitches within cooldown cannot farm capital");
    pitch.beginWeek(); pitch.closeWeek(); check(pitch.state().ledger.revenue == 0, "Capital is not revenue");
  }
  check(funded > 0 && rejected > 0, "Investment has seeded acceptance and rejection");
  company::Game burn(1); choose(burn, "hire", 0); choose(burn, "hire", 3); choose(burn, "hire", 4);
  choose(burn, "recovery", 4); choose(burn, "api", 1); choose(burn, "encoding", 2);
  for (int i = 0; i < 26 && !burn.bankrupt(); ++i) { burn.beginWeek(); burn.closeWeek(); }
  check(burn.bankrupt(), "Expensive development must be able to bankrupt company");
}
void security() {
  int unsafe = 0, protectedCount = 0;
  for (int seed = 0; seed < 100; ++seed) {
    company::Game a(seed), b(seed); launch(a); launch(b);
    choose(b, "identity", 1); choose(b, "delivery", 1);
    a.beginWeek(); b.beginWeek(); a.event(); b.event();
    unsafe += b.state().breaches; protectedCount += a.state().breaches;
  }
  check(unsafe > 0 && protectedCount == 0, "Early exposure must drive future breaches");
}
void architecture() {
  company::Game g(17);
  int total = 0;
  for (const auto& n : g.architectureNodes()) {
    total += n.monthlyCost;
    check(g.nodeDecision(n.label) == n.decision, "Graph labels must resolve to real configuration decisions");
  }
  check(total == g.cloudCost(), "Graph costs must reconcile with cloud billing");
  check(g.architecture().find("Server (undecided)") != std::string::npos, "Graph must show the active service");
  choose(g, "api", 3);
  check(g.architecture().find("Fargate") != std::string::npos, "Compute migration must update graph");
  check(g.assessArchitecture("surge").days == 0, "Bounded Fargate can handle this workload");
  choose(g, "storage", 3);
  check(g.assessArchitecture("archive").days > 0, "Deep Archive cannot serve a live catalogue instantly");
  choose(g, "storage", 2);
  check(g.assessArchitecture("archive").cost > 0, "IA retrieval charges must affect hot objects");
  choose(g, "storage", 4);
  check(g.assessArchitecture("zone").days > 0, "Single-zone EBS origin must create a zone dependency");
  choose(g, "delivery", 1);
  check(g.assessArchitecture("archive").days > 0, "S3 delivery cannot point at EBS without an origin");
  check(g.assessArchitecture("leak").breach, "Public paths must expose data");
  choose(g, "storage", 0); choose(g, "delivery", 0); choose(g, "database", 2);
  check(g.assessArchitecture("database").breach, "Public shared-credential DB must drive breaches");
  choose(g, "database", 0); choose(g, "monitoring", 1);
  check(g.assessArchitecture("leak").breach, "Secrets in logs must create a breach path");
  choose(g, "monitoring", 0); choose(g, "scaling", 1);
  check(g.assessArchitecture("surge").days > 0, "Fixed small capacity cannot absorb a surge");
  choose(g, "scaling", 2);
  check(g.assessArchitecture("surge").cost > 0, "Aggressive scaling must cause burst bills");
  const int cash = g.state().cash, days = g.state().lostDays, breaches = g.state().breaches;
  g.exercise("S");
  check(g.state().cash == cash - 200 && g.state().lostDays == days && g.state().breaches == breaches,
        "Tabletop must charge only its fee, not production consequences");
  choose(g, "api", 0); launch(g);
  const int before = g.state().cash;
  choose(g, "api", 3);
  check(g.state().cash == before - 900, "Production rewrites must have a migration cost");
  choose(g, "api", 3);
  check(g.state().cash == before - 900, "Selecting an unchanged node must not charge a migration");
  choose(g, "recovery", 2); choose(g, "drill", 0); choose(g, "recovery", 2);
  check(g.state().testedRecovery, "Unchanged recovery must keep its rehearsal");
  g.beginWeek();
  check(g.rivalReport().find("saves $") != std::string::npos, "Rival must reveal actual savings against an idle EC2/Fargate API");
  choose(g, "rival", 0);
  check(g.state().api == 0, "Rival advice must be actionable");
  company::Game video(3), game(3, company::Business::Game);
  check(game.architecture().find("Fargate session tasks") != std::string::npos, "Game architecture must use persistent workers");
  check(video.cloudCost() != game.cloudCost(), "Paths need different workload costs");
  choose(game, "encoding", 1);
  check(game.assessArchitecture("jobs").days > 0, "Persistent matches cannot be entire Lambda invocations");
  company::Game reckless(8); launch(reckless);
  choose(reckless,"storage",3); choose(reckless,"scaling",2); choose(reckless,"database",2);
  choose(reckless,"identity",1); choose(reckless,"monitoring",2); choose(reckless,"encoding",1);
  while (reckless.state().week < 52 && !reckless.bankrupt()) {
    reckless.beginWeek(); reckless.event(); reckless.closeWeek();
  }
  check(reckless.bankrupt() && reckless.state().breaches > 0, "Bad architecture must cause bankruptcy and data breaches in a live campaign");
}

void teamProposals() {
  int cheapGood=0, seniorGood=0, humanContext=0, agentContext=0;
  for(int seed=0;seed<60;++seed) {
    company::Game cheap(seed), senior(seed), human(seed);
    choose(cheap,"hire",2); choose(senior,"hire",4); choose(human,"hire",0);
    auto d=cheap.decision("identity");
    auto proposal=cheap.proposal(d);
    check(proposal.option==cheap.proposal(d).option && proposal.speaker==cheap.proposal(d).speaker,
      "Reopening proposals must not reroll them");
    check(proposal.speaker.find("Pip")!=std::string::npos && !proposal.text.empty(),
      "Proposals identify their actual author and use narration");
    cheapGood+=proposal.option==0;
    seniorGood+=senior.proposal(senior.decision("identity")).option==0;
    humanContext+=human.proposal(human.decision("import_policy")).option==0;
    agentContext+=senior.proposal(senior.decision("import_policy")).option==0;
    check(cheap.state().scopedIdentity && cheap.state().cash==85000,
      "Proposing does not apply or charge for a decision");
    cheap.choose(d,proposal.option);
    check(cheap.state().scopedIdentity==(proposal.option==0), "Accepting applies the proposed real architecture choice");
  }
  check(seniorGood>cheapGood && cheapGood>0 && cheapGood<60,
    "Technical ability must affect recommendations without guaranteeing correctness");
  check(humanContext>agentContext, "Human contextual ability must affect contextual recommendations");
}

void projectPlanning() {
  company::Game one(42), same(42), other(43);
  check(one.candidate(0).name == same.candidate(0).name && one.candidate(0).monthlyCost == same.candidate(0).monthlyCost,
        "Applicants must replay with the seed");
  check(one.candidate(0).name != other.candidate(0).name, "Seeds must vary employee names");
  check(one.candidate(0).name != one.candidate(1).name && one.candidate(0).age >= 18, "Applicants must be distinct adults");
  check(one.forecast().revenue == 0 && one.forecast().cloud > 0,
        "The startup must pay for its development environment without subscription revenue");
  one.beginWeek(); one.prepareWork(); one.resolveWork(); one.closeWeek();
  check(one.state().progress == 0, "Waiting must never build the system");
  check(one.dashboard().find("Product ") == std::string::npos && one.taskBoard().find("%") == std::string::npos,
        "Daily views must not show numeric delivery progress");
  check(one.monthlyReview().find("System delivery") == std::string::npos, "No numeric report before first month");
  one.beginWeek(); choose(one,"plan:Founder",0);
  check(one.state().assignments.size() == 1, "A worker must have one assignment");
  choose(one,"plan:Founder",0);
  check(one.state().assignments.size() == 1, "Duplicate planning must not give a worker two tasks");
  one.prepareWork();
  auto questions = one.questions();
  check(questions.size() == 1 && questions[0].id == "task:0", "Implementation prompt must belong to assigned task");
  one.choose(questions[0],0); one.resolveWork(); one.closeWeek();
  check(one.state().tasks[0].progress > 0 && !one.readyToLaunch(), "Partial work is not a launchable product");
  bool rejected = false;
  try { one.resolveWork(); } catch (const std::logic_error&) { rejected = true; }
  check(rejected, "Work cannot be counted twice");
  while(one.state().week < 4) { one.beginWeek(); one.prepareWork(); one.resolveWork(); one.closeWeek(); }
  check(one.monthlyReview().find("System delivery:") != std::string::npos, "Monthly review must include measured progress");
  check(one.monthlyReview().find("ARCHITECTURE REPORT / week 4") != std::string::npos,
    "Monthly reporting must snapshot the current architecture");
  company::Game departed(7); choose(departed,"hire",3);
  departed.beginWeek(); choose(departed,"plan:Ada",0); departed.prepareWork();
  choose(departed,"release",0); departed.resolveWork(); departed.closeWeek();
  check(departed.state().tasks[0].progress == 0, "A dismissed worker cannot deliver ghost work");
  int cheapQuestions = 0, seniorQuestions = 0, hiddenErrors = 0;
  for(int seed = 0; seed < 100; ++seed) {
    company::Game cheap(seed,company::Business::Game), senior(seed,company::Business::Game);
    choose(cheap,"hire",2); choose(senior,"hire",4); choose(cheap,"review",1);
    cheap.beginWeek(); senior.beginWeek();
    choose(cheap,"plan:Pip",0); choose(senior,"plan:Atlas",0);
    cheap.prepareWork(); senior.prepareWork();
    cheapQuestions += cheap.questions().size(); seniorQuestions += senior.questions().size();
    if(cheap.questions().empty()) {
      auto text = cheap.resolveWork();
      if(cheap.state().database == 2) {
        ++hiddenErrors;
        check(text.find("breach") == std::string::npos && cheap.assessArchitecture("database").breach,
              "Hidden implementation errors must connect to later incidents without immediate warning");
      }
    }
  }
  check(cheapQuestions > seniorQuestions && hiddenErrors > 0, "Capability must affect questions and autonomous decisions");
}

}
int main() {
  try {
    company::Game a(42), b(42), c(43);
    const auto story = play(a);
    check(story == play(b) && story != play(c), "Seeded replay must be deterministic and varied");
    check(a.state().week == 52 && !a.bankrupt(), "A prudent campaign must be viable");
    teamProposals(); editableTopology(); rewriteRules(); lifecycle(); recovery(); staffing(); salaryAndMarket(); financesAndInvestment(); security(); architecture(); projectPlanning();
    std::cout << "Weekly company: phases, consequences, payroll, recovery, resignations, raises, models, financing and replay passed.\n";
    return 0;
  } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
